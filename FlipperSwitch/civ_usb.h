#pragma once
#include <furi.h>

bool civ_usb_start(void);
void civ_usb_stop(void);
bool civ_usb_connected(void);
bool civ_usb_lost(void);
// Returns only after both reports have been received by the USB host.
bool civ_usb_send(const uint8_t keyboard[8], uint16_t buttons);
