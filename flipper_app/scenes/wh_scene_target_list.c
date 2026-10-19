#include "../wavehound_i.h"

#include <stdio.h>

#define ITEM_SEARCH_AGAIN 0
/* Result i is submenu index i + 1. */

/* Rows already added to the submenu, so a tick only appends the newcomers and
 * never touches existing rows (the submenu keeps the user's highlight put). */
static size_t s_shown;
static size_t s_header_count; /* count last written to the header */

static void wh_scene_target_list_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

static void format_label(const WhTarget* t, char* out, size_t size) {
    if(t->is_ap) {
        /* hidden networks have no SSID: show the BSSID */
        snprintf(out, size, "%d c%u %s", t->rssi, t->channel, t->name[0] ? t->name : t->mac);
    } else {
        /* '*' marks a randomized (privacy) MAC */
        snprintf(out, size, "%d c%u %s%s", t->rssi, t->channel, t->mac, t->randomized ? "*" : "");
    }
}

static const char* wh_scene_target_list_noun(WhSearchKind kind) {
    return kind == WhSearchAccessPoints ? "networks" :
           kind == WhSearchClients      ? "clients" :
                                          "devices";
}

/* Build the DISCOVER command for a continuous (seconds = 0) listen, mirroring the
 * channel rules of the old timed Searching scene: AP listens always hop; device
 * listens may be parked on one channel; client listens park on the AP's channel. */
static void wh_scene_target_list_send_discover(WavehoundApp* app) {
    const WhSearchKind kind = app->search_kind;
    const bool aps = kind == WhSearchAccessPoints;
    const bool clients = kind == WhSearchClients;
    const uint8_t channel = aps ? 0 : clients ? app->clients_ap.channel : app->device_channel;

    char cmd[48];
    if(clients) {
        snprintf(cmd, sizeof(cmd), "DISCOVER,0,CLIENTS,%u,%s", channel, app->clients_ap.mac);
    } else {
        snprintf(cmd, sizeof(cmd), "DISCOVER,0,%s,%u", aps ? "AP" : "STA", channel);
    }
    wh_send(app, cmd);
}

/* Header text: either the AP-picker prompt, or a live "scanning" count. */
static void wh_scene_target_list_set_header(WavehoundApp* app, size_t count) {
    char header[40];
    if(app->picking_ap_channel) {
        strlcpy(
            header,
            app->channel_purpose == WhChannelForDeviceSearch ? "Clients of which AP:" :
                                                               "Use channel of:",
            sizeof(header));
    } else if(!app->uart) {
        strlcpy(header, "USART busy!", sizeof(header));
    } else {
        snprintf(
            header, sizeof(header), "Scanning... %u %s", (unsigned)count,
            wh_scene_target_list_noun(app->search_kind));
    }
    submenu_set_header(app->submenu, header);
}

/* Clear the list and (re)start a continuous scan. */
static void wh_scene_target_list_restart(WavehoundApp* app) {
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    app->result_count = 0;
    app->result_total = 0;
    app->progress_found = 0;
    furi_mutex_release(app->results_mutex);
    s_shown = 0;
    s_header_count = 0;

    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu, "~ Search again", ITEM_SEARCH_AGAIN, wh_scene_target_list_callback, app);
    wh_scene_target_list_set_header(app, 0);

    if(app->uart) wh_scene_target_list_send_discover(app);
}

void wh_scene_target_list_on_enter(void* context) {
    WavehoundApp* app = context;
    wh_scene_target_list_restart(app);
    submenu_set_selected_item(app->submenu, ITEM_SEARCH_AGAIN);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_target_list_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        /* Append devices heard since the last tick; never rebuild, so the
         * highlight stays where the user left it. */
        char label[64];
        furi_mutex_acquire(app->results_mutex, FuriWaitForever);
        const size_t count = app->result_count;
        for(; s_shown < count; s_shown++) {
            format_label(&app->results[s_shown], label, sizeof(label));
            submenu_add_item(
                app->submenu, label, s_shown + 1, wh_scene_target_list_callback, app);
        }
        furi_mutex_release(app->results_mutex);
        if(count != s_header_count) {
            s_header_count = count;
            wh_scene_target_list_set_header(app, count);
        }
        return false;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == ITEM_SEARCH_AGAIN) {
        wh_scene_target_list_restart(app);
        return true;
    }

    WhTarget picked_target;
    bool picked = false;
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    if(event.event - 1 < app->result_count) {
        picked_target = app->results[event.event - 1];
        picked = true;
    }
    furi_mutex_release(app->results_mutex);
    if(!picked) return false;

    if(app->picking_ap_channel) {
        if(app->channel_purpose == WhChannelForDeviceSearch) {
            /* Device search scoped to this AP: list the stations associated with
             * it, on its channel. The next scan reads clients_ap. */
            app->clients_ap = picked_target;
            app->device_search_clients = true;
        } else {
            /* MAC hunt only needs the channel number to pin the typed MAC. */
            app->target.channel = picked_target.channel;
            app->target.pin_channel = true;
        }
        app->picking_ap_channel = false;
        app->channel_pick_pending = true;
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, WhSceneChannelSelect);
        return true;
    }

    app->target = picked_target;
    /* A device search parked on a channel by the user keeps the hunt there too;
     * a client is always on its AP's channel, so pin the hunt there as well. */
    app->target.pin_channel =
        (app->search_kind == WhSearchDevices && app->device_channel != 0) ||
        app->search_kind == WhSearchClients;

    /* Pre-fill manual entry with the last target. */
    wh_parse_mac(app->target.mac, app->mac_bytes);
    scene_manager_next_scene(app->scene_manager, WhSceneDetails);
    return true;
}

void wh_scene_target_list_on_exit(void* context) {
    WavehoundApp* app = context;
    /* Stop the board scanning whenever we leave (pick, back or restart handles
     * its own resend). Harmless once the board is idle. */
    wh_send(app, "STOP");
    submenu_reset(app->submenu);
}
