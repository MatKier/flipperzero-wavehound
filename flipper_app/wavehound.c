/*
 * Wavehound - WiFi fox hunting with the Flipper Zero WiFi dev board.
 *
 * Hunt an access point or any other device found during a timed listen, or a
 * MAC typed in by hand. The board reports the target's signal
 * strength; the Flipper turns it into a meter, a graph and Geiger clicks.
 */
#include "wavehound_i.h"
#include "wh_store.h"

#include <furi_hal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Wavehound"

/* ---------- helpers shared with scenes ---------- */

void wh_send(WavehoundApp* app, const char* line) {
    if(app->uart) wh_uart_send(app->uart, line);
}

static int hex_nibble(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wh_parse_mac(const char* str, uint8_t mac[6]) {
    for(int i = 0; i < 6; i++) {
        int hi = hex_nibble(str[i * 3]);
        int lo = hi < 0 ? -1 : hex_nibble(str[i * 3 + 1]);
        if(lo < 0) return false;
        if(i < 5 && str[i * 3 + 2] != ':') return false;
        mac[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

/* ---------- board line parsing (UART worker thread) ---------- */

static char* next_field(char** cursor) {
    char* start = *cursor;
    if(!start) return NULL;
    char* comma = strchr(start, ',');
    if(comma) {
        *comma = '\0';
        *cursor = comma + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static int field_int(char** cursor) {
    char* f = next_field(cursor);
    return f ? atoi(f) : 0;
}

static void wh_results_reset(WavehoundApp* app) {
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    app->result_count = 0;
    app->result_total = 0;
    app->progress_found = 0;
    furi_mutex_release(app->results_mutex);
}

/* Find an existing result by MAC, or reserve a new slot for it. Returns NULL
 * only when the table is full. Caller must release results_mutex. Continuous
 * discovery may re-send a MAC, and the live list appends in discovery order, so
 * matching keeps each device in its original slot instead of duplicating it. */
static WhTarget* wh_results_slot(WavehoundApp* app, const char* mac) {
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    for(size_t i = 0; i < app->result_count; i++) {
        if(strcmp(app->results[i].mac, mac) == 0) return &app->results[i];
    }
    if(app->result_count >= WH_MAX_RESULTS) return NULL;
    WhTarget* t = &app->results[app->result_count++];
    memset(t, 0, sizeof(WhTarget));
    return t;
}

static void wh_on_line(char* line, void* context) {
    WavehoundApp* app = context;
    char* cur = line;
    const char* tag = next_field(&cur);

    if(strcmp(tag, "H") == 0) {
        uint8_t ch = field_int(&cur);
        uint32_t frames = field_int(&cur);
        int8_t last = field_int(&cur);
        int8_t avg = field_int(&cur);
        int8_t max = field_int(&cur);
        bool locked = field_int(&cur) != 0;
        wh_hunt_view_push_sample(app->hunt, ch, frames, last, avg, max, locked);
    } else if(strcmp(tag, "S") == 0) {
        uint8_t ch = field_int(&cur);
        uint32_t frames = field_int(&cur);
        uint32_t bytes = field_int(&cur);
        int8_t rssi = field_int(&cur);
        wh_spectrum_view_push_channel(app->spectrum, ch, frames, bytes, rssi);
    } else if(strcmp(tag, "U") == 0) {
        wh_spectrum_view_push_unique(app->spectrum, field_int(&cur));
    } else if(strcmp(tag, "DISCOVER_BEGIN") == 0) {
        wh_results_reset(app);
    } else if(strcmp(tag, "DEV") == 0) {
        /* DEV,<mac>,<ch>,<rssi>,<frames>,<flags>,<ssid...> */
        const char* mac = next_field(&cur);
        uint8_t ch = field_int(&cur);
        int8_t rssi = field_int(&cur);
        uint16_t frames = field_int(&cur);
        const char* flags = next_field(&cur);
        if(!mac || !flags) return;
        WhTarget* t = wh_results_slot(app, mac);
        if(t) {
            strlcpy(t->mac, mac, sizeof(t->mac));
            strlcpy(t->name, cur ? cur : "", sizeof(t->name));
            t->channel = ch;
            t->rssi = rssi;
            t->frames = frames;
            t->is_ap = strchr(flags, 'A') != NULL;
            t->randomized = strchr(flags, 'R') != NULL;
        }
        furi_mutex_release(app->results_mutex);
    } else if(strcmp(tag, "INFO_BEGIN") == 0) {
        furi_mutex_acquire(app->results_mutex, FuriWaitForever);
        furi_string_set(app->board_info, "\e#Board info\n");
        furi_mutex_release(app->results_mutex);
    } else if(strcmp(tag, "BI") == 0) {
        /* BI,<label>,<value...> -- value is the rest of the line (no commas). */
        const char* label = next_field(&cur);
        if(label) {
            furi_mutex_acquire(app->results_mutex, FuriWaitForever);
            furi_string_cat_printf(app->board_info, "%s: %s\n", label, cur ? cur : "");
            furi_mutex_release(app->results_mutex);
        }
    } else if(strcmp(tag, "INFO_END") == 0) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventBoardInfoReady);
    } else if(strcmp(tag, "DP") == 0) {
        field_int(&cur); /* elapsed seconds; the progress view keeps its own clock */
        app->progress_found = field_int(&cur);
    } else if(strcmp(tag, "DISCOVER_END") == 0) {
        field_int(&cur); /* sent */
        app->result_total = field_int(&cur);
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventResultsReady);
    }
    /* READY / PONG / OK / ERR only refresh the liveness timestamp. */
}

/* Hunt view asks (Right key) to save the current target. */
static void wh_hunt_save_cb(void* context) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WhEventSaveTarget);
}

/* ---------- dispatcher plumbing ---------- */

static bool wh_custom_event_callback(void* context, uint32_t event) {
    WavehoundApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool wh_back_event_callback(void* context) {
    WavehoundApp* app = context;
    return scene_manager_handle_back_event(app->scene_manager);
}

static void wh_tick_event_callback(void* context) {
    WavehoundApp* app = context;
    app->board_alive = app->uart && wh_uart_board_alive(app->uart);
    scene_manager_handle_tick_event(app->scene_manager);
}

static WavehoundApp* wh_app_alloc(void) {
    WavehoundApp* app = malloc(sizeof(WavehoundApp));
    memset(app, 0, sizeof(WavehoundApp));
    app->results_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->board_info = furi_string_alloc();

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&wh_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, wh_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, wh_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, wh_tick_event_callback, WH_TICK_MS);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WhViewSubmenu, submenu_get_view(app->submenu));
    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WhViewByteInput, byte_input_get_view(app->byte_input));
    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WhViewTextInput, text_input_get_view(app->text_input));
    app->widget = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, WhViewWidget, widget_get_view(app->widget));
    app->hunt = wh_hunt_view_alloc();
    wh_hunt_view_set_save_callback(app->hunt, wh_hunt_save_cb, app);
    view_dispatcher_add_view(app->view_dispatcher, WhViewHunt, wh_hunt_view_get_view(app->hunt));
    app->spectrum = wh_spectrum_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WhViewSpectrum, wh_spectrum_view_get_view(app->spectrum));

    wh_store_load(app);

    return app;
}

static void wh_app_free(WavehoundApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, WhViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewHunt);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewSpectrum);
    submenu_free(app->submenu);
    byte_input_free(app->byte_input);
    text_input_free(app->text_input);
    widget_free(app->widget);
    wh_hunt_view_free(app->hunt);
    wh_spectrum_view_free(app->spectrum);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);
    furi_mutex_free(app->results_mutex);
    furi_string_free(app->board_info);
    free(app);
}

static bool wh_board_power_on(WavehoundApp* app) {
    if(furi_hal_power_is_otg_enabled()) return true;
    /* The boost converter sometimes needs a couple of attempts. */
    for(int attempt = 0; attempt < 5; attempt++) {
        if(furi_hal_power_enable_otg()) {
            app->otg_enabled_by_us = true;
            return true;
        }
        furi_delay_ms(10);
    }
    return false;
}

int32_t wavehound_app(void* p) {
    UNUSED(p);
    WavehoundApp* app = wh_app_alloc();

    /* The expansion service also listens on the USART; take it over. */
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);
    if(!wh_board_power_on(app)) FURI_LOG_W(TAG, "could not enable 5V on GPIO");

    app->uart = wh_uart_alloc(wh_on_line, app);
    if(app->uart) {
        wh_send(app, "STOP");
    } else {
        FURI_LOG_E(TAG, "USART busy");
    }

    scene_manager_next_scene(app->scene_manager, WhSceneStart);
    view_dispatcher_run(app->view_dispatcher);

    if(app->uart) {
        wh_send(app, "STOP");
        wh_uart_free(app->uart);
    }
    if(app->otg_enabled_by_us) furi_hal_power_disable_otg();
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    wh_app_free(app);
    return 0;
}
