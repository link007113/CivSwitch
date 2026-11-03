// Custom composite HID transport using Flipper's public USB interface API.
#include "civ_usb.h"
#include "civ_descriptors.h"
#include <furi_hal_usb.h>
#include <usb_hid.h>
#include <string.h>

#define PAD_IN     0x81
#define PAD_OUT    0x02
#define KEY_IN     0x83
#define KEY_OUT    0x03
#define TX_TIMEOUT 500

_Static_assert(sizeof(switch_usb_device_descriptor) == 18, "device descriptor");
_Static_assert(sizeof(switch_usb_configuration_descriptor) == 73, "configuration descriptor");
_Static_assert(sizeof(switch_usb_report_descriptor) == 86, "gamepad descriptor");
_Static_assert(sizeof(keyboard_usb_report_descriptor) == 63, "keyboard descriptor");

static usbd_device* device;
static FuriHalUsbInterface* previous;
static FuriSemaphore* tx_ready[2];
static volatile bool configured;
static volatile bool awake;
static volatile bool lost;
static bool ever_configured;
static bool started;
static uint8_t reports[2][8];
static uint8_t idle[2];
static uint8_t protocol[2];
static uint8_t led_state;

// Keep the Pico's USB identity; indices 1 and 2 are the Flipper HAL's
// manufacturer and product slots too. No serial-number string is advertised.
static const uint8_t manufacturer[] = {
    34, 3,   'P', 0,   'I', 0,   'C', 0,   'O', 0,   ' ', 0,   'S', 0,   'W', 0,   'I',
    0,  'T', 0,   'C', 0,   'H', 0,   ' ', 0,   'P', 0,   'R', 0,   'O', 0,   '.', 0,
};
static const uint8_t product[] = {
    30, 3,   'J', 0,   '2', 0,   '4', 0,   ' ', 0,   'C', 0,   'O', 0,   'N',
    0,  'T', 0,   'R', 0,   'O', 0,   'L', 0,   'L', 0,   'E', 0,   'R', 0,
};

static void endpoint(usbd_device* dev, uint8_t event, uint8_t ep) {
    if(event == usbd_evt_eptx) {
        if((ep & 0x7f) == 1) furi_semaphore_release(tx_ready[0]);
        if((ep & 0x7f) == 3) furi_semaphore_release(tx_ready[1]);
    } else if(event == usbd_evt_eprx) {
        uint8_t data[64];
        int32_t length = usbd_ep_read(dev, ep, data, sizeof(data));
        if((ep & 0x7f) == 3 && length > 0) led_state = data[0];
        // PicoSwitch echoes controller OUT reports when the IN endpoint is free.
        if((ep & 0x7f) == 2 && length > 0 &&
           furi_semaphore_acquire(tx_ready[0], 0) == FuriStatusOk) {
            if(usbd_ep_write(dev, PAD_IN, data, length) < 0) {
                furi_semaphore_release(tx_ready[0]);
            }
        }
    }
}

static usbd_respond configure(usbd_device* dev, uint8_t cfg) {
    if(ever_configured) lost = true;
    configured = false;
    const uint8_t endpoints[] = {PAD_IN, PAD_OUT, KEY_IN, KEY_OUT};
    if(cfg == 0) {
        for(size_t i = 0; i < COUNT_OF(endpoints); ++i) {
            usbd_ep_deconfig(dev, endpoints[i]);
            usbd_reg_endpoint(dev, endpoints[i], NULL);
        }
        return usbd_ack;
    }
    if(cfg != 1) return usbd_fail;
    for(size_t i = 0; i < COUNT_OF(endpoints); ++i) {
        if(!usbd_ep_config(dev, endpoints[i], USB_EPTYPE_INTERRUPT, i < 2 ? 64 : 8)) {
            return usbd_fail;
        }
        usbd_reg_endpoint(dev, endpoints[i], endpoint);
    }
    // A bus reset may have discarded an in-flight transfer.
    furi_semaphore_release(tx_ready[0]);
    furi_semaphore_release(tx_ready[1]);
    protocol[0] = protocol[1] = 1;
    idle[0] = idle[1] = 0;
    awake = configured = true;
    ever_configured = true;
    return usbd_ack;
}

static usbd_respond control(usbd_device* dev, usbd_ctlreq* req, usbd_rqc_callback* cb) {
    UNUSED(cb);
    if(req->wIndex > 1) return usbd_fail;
    const uint8_t i = req->wIndex;
    uint8_t type = req->bmRequestType & (USB_REQ_RECIPIENT | USB_REQ_TYPE);
    if(type == (USB_REQ_INTERFACE | USB_REQ_STANDARD) && req->bRequest == USB_STD_GET_DESCRIPTOR) {
        if((req->wValue >> 8) == USB_DTYPE_HID_REPORT) {
            dev->status.data_ptr = i ? keyboard_usb_report_descriptor :
                                       switch_usb_report_descriptor;
            dev->status.data_count = i ? sizeof(keyboard_usb_report_descriptor) :
                                         sizeof(switch_usb_report_descriptor);
        } else if((req->wValue >> 8) == USB_DTYPE_HID) {
            dev->status.data_ptr = switch_usb_configuration_descriptor + (i ? 50 : 18);
            dev->status.data_count = 9;
        } else {
            return usbd_fail;
        }
        return usbd_ack;
    }
    if(type != (USB_REQ_INTERFACE | USB_REQ_CLASS)) return usbd_fail;
    switch(req->bRequest) {
    case USB_HID_SETIDLE:
        idle[i] = req->wValue >> 8;
        return usbd_ack;
    case USB_HID_GETIDLE:
        dev->status.data_ptr = &idle[i];
        dev->status.data_count = 1;
        return usbd_ack;
    case USB_HID_SETPROTOCOL:
        if(req->wValue > 1) return usbd_fail;
        protocol[i] = req->wValue;
        return usbd_ack;
    case USB_HID_GETPROTOCOL:
        dev->status.data_ptr = &protocol[i];
        dev->status.data_count = 1;
        return usbd_ack;
    case USB_HID_GETREPORT:
        if((req->wValue >> 8) == 2 && i == 1) {
            dev->status.data_ptr = &led_state;
            dev->status.data_count = 1;
        } else if((req->wValue >> 8) == 1) {
            dev->status.data_ptr = reports[i];
            dev->status.data_count = 8;
        } else {
            return usbd_fail;
        }
        return usbd_ack;
    case USB_HID_SETREPORT:
        // libusb_stm32 supplies the completed DATA-OUT stage in req->data.
        if(i == 1 && req->wLength == 1 && (req->wValue >> 8) == 2) {
            led_state = req->data[0];
            return usbd_ack;
        }
        return usbd_fail;
    default:
        return usbd_fail;
    }
}

static void init(usbd_device* dev, FuriHalUsbInterface* intf, void* context) {
    UNUSED(context);
    // The host must see the same EP0 packet size the HAL actually transmits.
    // Advertising the Pico's 64 bytes with Flipper's 8-byte EP0 truncates
    // descriptor reads at the host and prevents SET_CONFIGURATION entirely.
    intf->dev_descr->bMaxPacketSize0 = dev->status.ep0size;
    device = dev;
    usbd_reg_config(dev, configure);
    usbd_reg_control(dev, control);
    usbd_connect(dev, true);
}

static void deinit(usbd_device* dev) {
    configured = awake = false;
    configure(dev, 0);
    usbd_reg_config(dev, NULL);
    usbd_reg_control(dev, NULL);
    device = NULL;
}

static void wakeup(usbd_device* dev) {
    UNUSED(dev);
    awake = true;
}

static void suspend(usbd_device* dev) {
    UNUSED(dev);
    awake = false;
    if(ever_configured) lost = true;
}

static FuriHalUsbInterface interface = {
    .init = init,
    .deinit = deinit,
    .wakeup = wakeup,
    .suspend = suspend,
    .dev_descr = (struct usb_device_descriptor*)switch_usb_device_descriptor,
    .str_manuf_descr = (void*)manufacturer,
    .str_prod_descr = (void*)product,
    .str_serial_descr = NULL,
    .cfg_descr = switch_usb_configuration_descriptor,
};

bool civ_usb_start(void) {
    if(started || furi_hal_usb_is_locked()) return false;
    previous = furi_hal_usb_get_config();
    memset(reports, 0, sizeof(reports));
    const uint8_t neutral[8] = {0, 0, 8, 128, 128, 128, 128, 0};
    memcpy(reports[0], neutral, 8);
    for(size_t i = 0; i < 2; ++i)
        tx_ready[i] = furi_semaphore_alloc(1, 1);
    configured = awake = false;
    lost = ever_configured = false;
    if(!furi_hal_usb_set_config(&interface, NULL)) {
        for(size_t i = 0; i < 2; ++i)
            furi_semaphore_free(tx_ready[i]);
        return false;
    }
    furi_hal_usb_lock();
    started = true;
    return true;
}

bool civ_usb_connected(void) {
    return configured && awake && !lost && device &&
           device->status.device_state == usbd_state_configured;
}

bool civ_usb_lost(void) {
    return lost;
}

bool civ_usb_send(const uint8_t keyboard[8], uint16_t buttons) {
    if(!civ_usb_connected()) return false;
    const uint8_t pad[8] = {buttons & 0xff, buttons >> 8, 8, 128, 128, 128, 128, 0};
    const uint8_t* data[] = {pad, keyboard};
    const uint8_t eps[] = {PAD_IN, KEY_IN};
    for(size_t i = 0; i < 2; ++i) {
        if(furi_semaphore_acquire(tx_ready[i], TX_TIMEOUT) != FuriStatusOk) return false;
        int32_t length = -1;
        FURI_CRITICAL_ENTER();
        if(civ_usb_connected()) {
            memcpy(reports[i], data[i], 8);
            length = usbd_ep_write(device, eps[i], data[i], 8);
        }
        FURI_CRITICAL_EXIT();
        if(length != 8) {
            furi_semaphore_release(tx_ready[i]);
            return false;
        }
    }
    for(size_t i = 0; i < 2; ++i) {
        if(furi_semaphore_acquire(tx_ready[i], TX_TIMEOUT) != FuriStatusOk) return false;
        furi_semaphore_release(tx_ready[i]);
    }
    return civ_usb_connected();
}

void civ_usb_stop(void) {
    if(!started) return;
    const uint8_t released[8] = {0};
    civ_usb_send(released, 0);
    furi_hal_usb_unlock();
    // set_config synchronizes with deinit before freeing callback state.
    furi_check(furi_hal_usb_set_config(previous, NULL));
    // GetConfig is queued after the mode-change request, providing a barrier.
    furi_hal_usb_get_config();
    for(size_t i = 0; i < 2; ++i) {
        furi_semaphore_free(tx_ready[i]);
        tx_ready[i] = NULL;
    }
    started = false;
}
