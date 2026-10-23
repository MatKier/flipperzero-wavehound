/*
 * Wavehound - WiFi fox hunting with the Flipper Zero WiFi dev board.
 *
 * Hunt an access point or any other device found during a live scan, or a
 * MAC typed in by hand. The board reports the target's signal
 * strength; the Flipper turns it into a meter, a graph and Geiger clicks.
 */
#include "wavehound_i.h"
#include "wh_ap_store.h"
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

/* Hex-encode a C string (lowercase, NUL-terminated); matches the board's
 * proto_hex_encode so free-text fields survive the comma-separated protocol. */
static void wh_hex_encode(const char* src, char* out, size_t out_sz) {
    static const char digits[] = "0123456789abcdef";
    size_t n = 0;
    for(const unsigned char* p = (const unsigned char*)src; *p && n + 2 < out_sz; p++) {
        out[n++] = digits[*p >> 4];
        out[n++] = digits[*p & 0x0f];
    }
    out[n] = '\0';
}

/* Decode an even-length hex string into out (NUL-terminated). Returns the byte
 * count, or -1 on malformed/oversized input. */
static int wh_hex_decode(const char* s, char* out, size_t out_sz) {
    size_t n = 0;
    while(s && s[0]) {
        int hi = hex_nibble(s[0]);
        int lo = hex_nibble(s[1]); /* s[1] may be the NUL terminator */
        if(hi < 0 || lo < 0) return -1;
        if(n + 1 >= out_sz) return -1;
        out[n++] = (char)(hi << 4 | lo);
        s += 2;
    }
    out[n] = '\0';
    return (int)n;
}

void wh_ap_connect(WavehoundApp* app) {
    char ssid_hex[2 * WH_AP_SSID_MAX];
    char psk_hex[2 * WH_AP_PSK_MAX];
    wh_hex_encode(app->clients_ap.name, ssid_hex, sizeof(ssid_hex));
    wh_hex_encode(app->psk_buf, psk_hex, sizeof(psk_hex));

    app->conn.connecting = true;
    app->conn.connected = false;
    app->conn.fail_reason = 0;
    app->conn.fail_msg[0] = '\0';
    strlcpy(app->conn.ssid, app->clients_ap.name, sizeof(app->conn.ssid));

    /* CONNECT,<ssid_hex>,<psk_hex>,<bssid>,<ch> -- pass the AP's BSSID/channel so
     * the join is quick and unambiguous. */
    char cmd[2 * WH_AP_SSID_MAX + 2 * WH_AP_PSK_MAX + 40];
    snprintf(
        cmd,
        sizeof(cmd),
        "CONNECT,%s,%s,%s,%u",
        ssid_hex,
        psk_hex,
        app->clients_ap.mac,
        app->clients_ap.channel);
    wh_send(app, cmd);
}

void wh_ap_login_proceed(WavehoundApp* app) {
    /* Continue from the login flow to the client list: collapse back to
     * ChannelSelect, which resumes with the pending pick and pushes the client
     * TargetList (the same path a plain channel pick takes). */
    app->channel_pick_pending = true;
    scene_manager_search_and_switch_to_previous_scene(app->scene_manager, WhSceneChannelSelect);
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

/* A new discovery started: clear the results, unless the scan is a resume
 * (returning from a result's Details), which keeps what was already found. */
static void wh_results_begin(WavehoundApp* app) {
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    if(app->results_keep_begins)
        app->results_keep_begins--;
    else
        app->result_count = 0;
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
    /* Apply (and consume) an IP that arrived before this row existed. */
    for(size_t i = 0; i < app->pending_ip_count; i++) {
        if(strcmp(app->pending_ips[i].mac, mac) == 0) {
            strlcpy(t->ip, app->pending_ips[i].ip, sizeof(t->ip));
            strlcpy(t->host, app->pending_ips[i].host, sizeof(t->host));
            app->pending_ips[i] = app->pending_ips[--app->pending_ip_count];
            break;
        }
    }
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
        int8_t nf = field_int(&cur); /* 0 from older firmware (field absent) */
        wh_hunt_view_push_sample(app->hunt, ch, frames, last, avg, max, locked, nf);
    } else if(strcmp(tag, "S") == 0) {
        uint8_t ch = field_int(&cur);
        uint32_t frames = field_int(&cur);
        uint32_t bytes = field_int(&cur);
        int8_t rssi = field_int(&cur);
        wh_spectrum_view_push_channel(app->spectrum, ch, frames, bytes, rssi);
    } else if(strcmp(tag, "U") == 0) {
        wh_spectrum_view_push_unique(app->spectrum, field_int(&cur));
    } else if(strcmp(tag, "DISCOVER_BEGIN") == 0) {
        wh_results_begin(app);
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
            /* Security + PHY codes (APs only; absent from older firmware). */
            const char* sec = strchr(flags, 'o') ? "Open" :
                              strchr(flags, 'w') ? "WEP" :
                              strchr(flags, '1') ? "WPA" :
                              strchr(flags, '2') ? "WPA2" :
                              strchr(flags, '3') ? "WPA3" : "";
            strlcpy(t->security, sec, sizeof(t->security));
            const char* phy = strchr(flags, 'n') ? "n" :
                              strchr(flags, 'c') ? "ac" :
                              strchr(flags, 'x') ? "ax" :
                              strchr(flags, 'g') ? "g" : "";
            strlcpy(t->phy, phy, sizeof(t->phy));
        }
        furi_mutex_release(app->results_mutex);
    } else if(strcmp(tag, "IP") == 0) {
        /* IP,<mac>,<ip>[,<host_hex>] -- annotate the matching client row and, if
         * it's the target we're hunting, the hunt screen. */
        const char* mac = next_field(&cur);
        const char* ip = next_field(&cur);
        if(!mac || !ip) return;
        char host[33] = "";
        const char* host_hex = next_field(&cur);
        if(host_hex) wh_hex_decode(host_hex, host, sizeof(host));

        furi_mutex_acquire(app->results_mutex, FuriWaitForever);
        bool matched = false;
        for(size_t i = 0; i < app->result_count; i++) {
            if(strcmp(app->results[i].mac, mac) == 0) {
                strlcpy(app->results[i].ip, ip, sizeof(app->results[i].ip));
                if(host[0]) strlcpy(app->results[i].host, host, sizeof(app->results[i].host));
                matched = true;
                break;
            }
        }
        if(!matched) {
            /* The row doesn't exist yet (sweep ran before the list opened, or the
             * ARP reply beat the client scan): remember it, keyed by MAC, and
             * wh_results_slot() applies it when the row appears. */
            WhPendingIp* p = NULL;
            for(size_t i = 0; i < app->pending_ip_count; i++) {
                if(strcmp(app->pending_ips[i].mac, mac) == 0) {
                    p = &app->pending_ips[i];
                    break;
                }
            }
            if(!p && app->pending_ip_count < WH_MAX_RESULTS) {
                p = &app->pending_ips[app->pending_ip_count++];
                p->host[0] = '\0';
            }
            if(p) {
                strlcpy(p->mac, mac, sizeof(p->mac));
                strlcpy(p->ip, ip, sizeof(p->ip));
                if(host[0]) strlcpy(p->host, host, sizeof(p->host));
            }
        }
        furi_mutex_release(app->results_mutex);

        if(strcmp(mac, app->target.mac) == 0) {
            strlcpy(app->target.ip, ip, sizeof(app->target.ip));
            if(host[0]) strlcpy(app->target.host, host, sizeof(app->target.host));
            wh_hunt_view_set_ip(app->hunt, app->target.ip, app->target.host);
        }
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
    } else if(strcmp(tag, "READY") == 0 || strcmp(tag, "PONG") == 0) {
        /* Capture the board firmware version so the login UI can require it. */
        const char* v = next_field(&cur);
        if(v) app->board_fw_version = atoi(v);
    } else if(strcmp(tag, "CONN") == 0) {
        /* CONN,GOTIP,<ip>,<gw>,<dns> | CONN,FAIL,<reason> | CONN,DOWN | CONN,ASSOC,... */
        const char* state = next_field(&cur);
        if(!state) return;
        if(strcmp(state, "GOTIP") == 0) {
            const char* ip = next_field(&cur);
            strlcpy(app->conn.ip, ip ? ip : "", sizeof(app->conn.ip));
            app->conn.connecting = false;
            app->conn.connected = true;
            view_dispatcher_send_custom_event(app->view_dispatcher, WhEventConnGotIp);
        } else if(strcmp(state, "FAIL") == 0) {
            app->conn.fail_reason = field_int(&cur);
            snprintf(app->conn.fail_msg, sizeof(app->conn.fail_msg), "wifi %d", app->conn.fail_reason);
            app->conn.connecting = false;
            app->conn.connected = false;
            view_dispatcher_send_custom_event(app->view_dispatcher, WhEventConnFail);
        } else if(strcmp(state, "DOWN") == 0) {
            app->conn.connecting = false;
            app->conn.connected = false;
            app->conn.ip[0] = '\0';
            /* The subnet is gone: any pending IPs are stale. */
            furi_mutex_acquire(app->results_mutex, FuriWaitForever);
            app->pending_ip_count = 0;
            furi_mutex_release(app->results_mutex);
        }
        /* ASSOC is layer-2 only: nothing to show until GOTIP. */
    } else if(strcmp(tag, "WPS") == 0) {
        /* WPS,OK,<ssid_hex>,<psk_hex> | WPS,FAIL,<reason> | WPS,ACTIVE */
        const char* state = next_field(&cur);
        if(!state) return;
        if(strcmp(state, "OK") == 0) {
            const char* ssid_hex = next_field(&cur);
            const char* psk_hex = next_field(&cur);
            char ssid[WH_AP_SSID_MAX] = "";
            if(ssid_hex) wh_hex_decode(ssid_hex, ssid, sizeof(ssid));
            if(ssid[0]) strlcpy(app->conn.ssid, ssid, sizeof(app->conn.ssid));
            if(psk_hex) wh_hex_decode(psk_hex, app->psk_buf, sizeof(app->psk_buf));
            app->login_pending_save = true; /* offer to save after the join succeeds */
            view_dispatcher_send_custom_event(app->view_dispatcher, WhEventWpsOk);
        } else if(strcmp(state, "FAIL") == 0) {
            view_dispatcher_send_custom_event(app->view_dispatcher, WhEventWpsFail);
        }
        /* ACTIVE: the WPS scene already shows "press the button". */
    } else if(strcmp(tag, "ERR") == 0) {
        /* A command was rejected. While connecting, it's our CONNECT (e.g. a bad
         * passphrase) -- surface it instead of waiting forever. */
        if(app->conn.connecting) {
            const char* reason = next_field(&cur);
            strlcpy(app->conn.fail_msg, reason ? reason : "rejected", sizeof(app->conn.fail_msg));
            app->conn.connecting = false;
            app->conn.connected = false;
            view_dispatcher_send_custom_event(app->view_dispatcher, WhEventConnFail);
        }
    }
    /* OK and other lines only refresh the liveness timestamp. */
}

/* Hunt view asks (Right key) to save the current target. */
static void wh_hunt_save_cb(void* context) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WhEventSaveTarget);
}

/* Hunt view asks (Down key) to toggle the elicitation ping. */
static void wh_hunt_elicit_cb(void* context) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WhEventElicitToggle);
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
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
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
    wh_hunt_view_set_elicit_callback(app->hunt, wh_hunt_elicit_cb, app);
    view_dispatcher_add_view(app->view_dispatcher, WhViewHunt, wh_hunt_view_get_view(app->hunt));
    app->spectrum = wh_spectrum_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WhViewSpectrum, wh_spectrum_view_get_view(app->spectrum));
    app->var_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, WhViewVarItemList, variable_item_list_get_view(app->var_item_list));

    wh_store_load(app);
    wh_ap_store_load(app);
    wh_settings_load(app);
    wh_settings_apply(app);

    return app;
}

static void wh_app_free(WavehoundApp* app) {
    view_dispatcher_remove_view(app->view_dispatcher, WhViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewWidget);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewHunt);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewSpectrum);
    view_dispatcher_remove_view(app->view_dispatcher, WhViewVarItemList);
    submenu_free(app->submenu);
    byte_input_free(app->byte_input);
    text_input_free(app->text_input);
    widget_free(app->widget);
    variable_item_list_free(app->var_item_list);
    wh_hunt_view_free(app->hunt);
    wh_spectrum_view_free(app->spectrum);

    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    /* Guarantee the hunt screen's "Keep backlight" enforce can never outlive
     * the app: scene_manager_free does not run the active scene's on_exit, so
     * a quit straight from the hunt screen would otherwise leave the backlight
     * forced on system-wide. Releasing here is a no-op if nothing enforced it. */
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);
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
        /* Prompt a PONG,<version> so we learn the board firmware early -- the AP
         * login UI is gated on it, and during a scan the keep-alive ping is
         * otherwise suppressed. */
        wh_send(app, "PING");
    } else {
        FURI_LOG_E(TAG, "USART busy");
    }

    scene_manager_next_scene(app->scene_manager, WhSceneStart);
    view_dispatcher_run(app->view_dispatcher);

    if(app->uart) {
        /* Leaving the app: park the board dark (STOP would leave the idle LED
         * lit, and the board stays powered on the Flipper's always-on 3V3). */
        wh_send(app, "SLEEP");
        wh_uart_free(app->uart);
    }
    if(app->otg_enabled_by_us) furi_hal_power_disable_otg();
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);

    wh_app_free(app);
    return 0;
}
