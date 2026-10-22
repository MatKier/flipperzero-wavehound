#include "../wavehound_i.h"
#include "../wh_ap_store.h"

#include <stdio.h>

/* Waits for the board to associate and get a lease after a CONNECT / WPS. On
 * success it either offers to save a freshly entered password, or continues
 * straight to the client list; on failure it offers to retry (Back) or skip. */

typedef enum {
    ApcConnecting,
    ApcFail,
    ApcSave,
} ApcState;

static ApcState s_state;
static uint32_t s_ticks; /* WH_TICK_MS ticks spent connecting */

/* Give up waiting after ~15 s (association + DHCP are normally a few seconds). */
#define AP_CONNECT_TIMEOUT_TICKS (15000 / WH_TICK_MS)

static void wh_scene_ap_connect_button_cb(GuiButtonType type, InputType it, void* context) {
    if(it != InputTypeShort) return;
    WavehoundApp* app = context;
    if(s_state == ApcSave) {
        view_dispatcher_send_custom_event(
            app->view_dispatcher, type == GuiButtonTypeRight ? WhEventApSave : WhEventApDiscard);
    } else if(s_state == ApcFail && type == GuiButtonTypeRight) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventApDiscard); /* skip */
    }
}

static const char* wh_scene_ap_connect_ssid(WavehoundApp* app) {
    return app->conn.ssid[0] ? app->conn.ssid : app->clients_ap.name;
}

static void wh_scene_ap_connect_render(WavehoundApp* app) {
    Widget* w = app->widget;
    widget_reset(w);
    char line[64];

    if(s_state == ApcConnecting) {
        widget_add_string_element(w, 64, 22, AlignCenter, AlignTop, FontPrimary, "Connecting...");
        snprintf(line, sizeof(line), "%s", wh_scene_ap_connect_ssid(app));
        widget_add_string_element(w, 64, 42, AlignCenter, AlignTop, FontSecondary, line);
    } else if(s_state == ApcFail) {
        widget_add_string_element(w, 64, 12, AlignCenter, AlignTop, FontPrimary, "Failed");
        snprintf(line, sizeof(line), "%s", app->conn.fail_msg[0] ? app->conn.fail_msg : "no response");
        widget_add_string_element(w, 64, 30, AlignCenter, AlignTop, FontSecondary, line);
        widget_add_string_element(w, 64, 42, AlignCenter, AlignTop, FontSecondary, "Back to retry");
        widget_add_button_element(w, GuiButtonTypeRight, "Skip", wh_scene_ap_connect_button_cb, app);
    } else { /* ApcSave */
        widget_add_string_element(w, 64, 10, AlignCenter, AlignTop, FontPrimary, "Connected");
        widget_add_string_element(
            w, 64, 28, AlignCenter, AlignTop, FontSecondary, "Save password for");
        snprintf(line, sizeof(line), "%s?", wh_scene_ap_connect_ssid(app));
        widget_add_string_element(w, 64, 40, AlignCenter, AlignTop, FontSecondary, line);
        widget_add_button_element(w, GuiButtonTypeLeft, "No", wh_scene_ap_connect_button_cb, app);
        widget_add_button_element(w, GuiButtonTypeRight, "Save", wh_scene_ap_connect_button_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewWidget);
}

static void wh_scene_ap_connect_success(WavehoundApp* app) {
    if(app->login_pending_save && app->psk_buf[0]) {
        s_state = ApcSave;
        wh_scene_ap_connect_render(app);
    } else {
        wh_ap_login_proceed(app); /* straight to the client list */
    }
}

void wh_scene_ap_connect_on_enter(void* context) {
    WavehoundApp* app = context;
    s_state = ApcConnecting;
    s_ticks = 0;
    wh_scene_ap_connect_render(app);
    /* The lease may already be in (fast join); handle it on the next loop turn so
     * we don't navigate from within on_enter. */
    if(app->conn.connected) {
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventConnGotIp);
    }
}

bool wh_scene_ap_connect_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        if(s_state == ApcConnecting && ++s_ticks > AP_CONNECT_TIMEOUT_TICKS) {
            snprintf(app->conn.fail_msg, sizeof(app->conn.fail_msg), "timeout");
            app->conn.connecting = false;
            s_state = ApcFail;
            wh_scene_ap_connect_render(app);
        }
        return false;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case WhEventConnGotIp:
        wh_scene_ap_connect_success(app);
        return true;
    case WhEventConnFail:
        s_state = ApcFail;
        wh_scene_ap_connect_render(app);
        return true;
    case WhEventApSave:
        wh_ap_store_add(
            app, wh_scene_ap_connect_ssid(app), app->clients_ap.security, app->psk_buf);
        app->login_pending_save = false;
        wh_ap_login_proceed(app);
        return true;
    case WhEventApDiscard:
        app->login_pending_save = false;
        wh_ap_login_proceed(app);
        return true;
    default:
        return false;
    }
}

void wh_scene_ap_connect_on_exit(void* context) {
    WavehoundApp* app = context;
    widget_reset(app->widget);
}
