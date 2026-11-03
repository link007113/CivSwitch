#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdio.h>
#include "civ_usb.h"
#include "civ_format.h"

#define PAYLOAD_PATH EXT_PATH("apps_data/civswitch/payload.civ")

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* input;
    FuriThread* worker;
    ViewPort* view;
    volatile bool cancel;
    bool running;
    char status[40];
    uint32_t step;
    uint32_t total;
    uint32_t line;
    uint32_t lines;
} CivApp;

typedef struct {
    File* file;
    uint8_t data[768];
    size_t pos;
    size_t size;
} CivReader;

static const uint8_t released[8] = {0};

static void status(CivApp* app, const char* message) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    snprintf(app->status, sizeof(app->status), "%s", message);
    furi_mutex_release(app->mutex);
}

static bool read_record(CivReader* reader, uint8_t record[CIV_RECORD_SIZE]) {
    if(reader->pos == reader->size) {
        reader->size = storage_file_read(reader->file, reader->data, sizeof(reader->data));
        reader->pos = 0;
    }
    if(reader->size - reader->pos < CIV_RECORD_SIZE) return false;
    memcpy(record, reader->data + reader->pos, CIV_RECORD_SIZE);
    reader->pos += CIV_RECORD_SIZE;
    return true;
}

static bool hold(CivApp* app, uint16_t ms, const uint8_t keyboard[8], uint16_t buttons) {
    if(app->cancel || !civ_usb_send(keyboard, buttons)) return false;
    // Count the minimum hold from confirmed delivery, never from an SD read.
    uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < furi_ms_to_ticks(ms)) {
        if(app->cancel || !civ_usb_connected()) return false;
        furi_delay_ms(1);
        if(furi_get_tick() - start < furi_ms_to_ticks(ms) && !civ_usb_send(keyboard, buttons))
            return false;
    }
    return true;
}

static int32_t playback(void* context) {
    CivApp* app = context;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    CivReader* reader = malloc(sizeof(CivReader));
    memset(reader, 0, sizeof(CivReader));
    reader->file = storage_file_alloc(storage);
    uint8_t header[CIV_HEADER_SIZE], record[CIV_RECORD_SIZE];
    const char* result = "Invalid payload.civ";
    status(app, "Checking SD payload...");
    if(!storage_file_open(reader->file, PAYLOAD_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        result = "Missing payload.civ on SD";
        goto cleanup;
    }
    if(storage_file_read(reader->file, header, sizeof(header)) != sizeof(header) ||
       !civ_header_valid(header, storage_file_size(reader->file)))
        goto cleanup;
    const uint32_t count = civ_u32(header + 8);
    uint32_t found_lines = 0;
    // Validate the complete file before sending anything to the console.
    for(uint32_t i = 0; i < count; ++i) {
        if(app->cancel) goto cleanup;
        if(!read_record(reader, record) || !civ_record_valid(record)) goto cleanup;
        if(record[4] == 0x29) ++found_lines;
    }
    if(found_lines != civ_u32(header + 12) || memcmp(record + 2, released, 8) ||
       civ_u16(record + 10) || !storage_file_seek(reader->file, CIV_HEADER_SIZE, true)) {
        goto cleanup;
    }
    reader->pos = reader->size = 0;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->total = count;
    app->lines = found_lines;
    app->step = app->line = 0;
    furi_mutex_release(app->mutex);
    status(app, "Waiting for USB host...");
    if(!civ_usb_start()) {
        result = "USB busy; close other app";
        goto cleanup;
    }
    while(!civ_usb_connected() && !civ_usb_lost() && !app->cancel)
        furi_delay_ms(20);
    if(app->cancel) goto usb_cleanup;
    if(civ_usb_lost()) {
        result = "USB lost; start again";
        goto usb_cleanup;
    }
    status(app, "Starting in 1 second...");
    if(!hold(app, 1000, released, 0)) {
        result = "USB lost; start again";
        goto usb_cleanup;
    }
    status(app, "Typing into Civ7...");
    for(uint32_t i = 0; i < count; ++i) {
        if(!read_record(reader, record)) {
            result = "SD read failed";
            goto usb_cleanup;
        }
        if(!hold(app, civ_u16(record), record + 2, civ_u16(record + 10))) {
            result = "USB lost; start again";
            goto usb_cleanup;
        }
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->step = i + 1;
        if(record[4] == 0x29) ++app->line;
        furi_mutex_release(app->mutex);
    }
    result = "Done; check Switch screen";
usb_cleanup:
    civ_usb_stop();
cleanup:
    storage_file_close(reader->file);
    storage_file_free(reader->file);
    free(reader);
    furi_record_close(RECORD_STORAGE);
    status(app, app->cancel ? "Stopped" : result);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->running = false;
    furi_mutex_release(app->mutex);
    return 0;
}

static void draw(Canvas* canvas, void* context) {
    CivApp* app = context;
    char text[48];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "CivSwitch USB");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 25, app->status);
    if(app->total) {
        snprintf(
            text,
            sizeof(text),
            "Line %lu/%lu   %lu%%",
            (unsigned long)app->line,
            (unsigned long)app->lines,
            (unsigned long)(app->step * 100 / app->total));
        canvas_draw_str(canvas, 2, 38, text);
    } else {
        canvas_draw_str(canvas, 2, 38, "Open Civ7 chat; USB to dock");
    }
    canvas_draw_str(
        canvas, 2, 52, app->running ? "BACK: stop and exit" : "OK: start   BACK: exit");
    furi_mutex_release(app->mutex);
}

static void input(InputEvent* event, void* context) {
    CivApp* app = context;
    furi_message_queue_put(app->input, event, 0);
}

int32_t civswitch_app(void* context) {
    UNUSED(context);
    CivApp* app = malloc(sizeof(CivApp));
    memset(app, 0, sizeof(CivApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view = view_port_alloc();
    status(app, "Ready (US keyboard layout)");
    view_port_draw_callback_set(app->view, draw, app);
    view_port_input_callback_set(app->view, input, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view, GuiLayerFullscreen);
    bool exit = false;
    while(!exit) {
        InputEvent event;
        if(furi_message_queue_get(app->input, &event, 50) == FuriStatusOk &&
           event.type == InputTypeShort) {
            if(event.key == InputKeyBack) {
                app->cancel = true;
                exit = true;
            } else if(event.key == InputKeyOk) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                bool running = app->running;
                furi_mutex_release(app->mutex);
                if(!running) {
                    if(app->worker) {
                        furi_thread_join(app->worker);
                        furi_thread_free(app->worker);
                    }
                    app->cancel = false;
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    app->running = true;
                    app->total = app->step = app->line = app->lines = 0;
                    furi_mutex_release(app->mutex);
                    app->worker = furi_thread_alloc_ex("CivPlayback", 4096, playback, app);
                    furi_thread_start(app->worker);
                }
            }
        }
        view_port_update(app->view);
    }
    if(app->worker) {
        status(app, "Stopping USB...");
        view_port_update(app->view);
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }
    view_port_enabled_set(app->view, false);
    gui_remove_view_port(gui, app->view);
    view_port_free(app->view);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
