#include "../wavehound_i.h"

#include <stdio.h>

#define ITEM_SEARCH_AGAIN 0
/* Result i is submenu index i + 1. */

/* Scene state: FRESH clears the list and starts a new scan on enter; RESUME
 * keeps the results already found (set when drilling into a result's Details,
 * so returning does not throw them away). "Search again" always starts fresh. */
#define TARGET_LIST_FRESH 0
#define TARGET_LIST_RESUME 1

/* Rows already added to the submenu, so a tick only appends the newcomers and
 * never touches existing rows (the submenu keeps the user's highlight put). */
static size_t s_shown;
static size_t s_header_count; /* count last written to the header */
static uint8_t s_spin; /* animated scan indicator phase */
static uint32_t s_picked_item; /* submenu index of the last picked result */

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

/* Build the DISCOVER command: AP listens always hop; device listens may be
 * parked on one channel; client listens park on the chosen AP's channel. */
static void wh_scene_target_list_send_discover(WavehoundApp* app) {
    const WhSearchKind kind = app->search_kind;
    const bool aps = kind == WhSearchAccessPoints;
    const bool clients = kind == WhSearchClients;
    const uint8_t channel = aps ? 0 : clients ? app->clients_ap.channel : app->device_channel;

    char cmd[48];
    if(clients) {
        snprintf(cmd, sizeof(cmd), "DISCOVER,CLIENTS,%u,%s", channel, app->clients_ap.mac);
    } else {
        snprintf(cmd, sizeof(cmd), "DISCOVER,%s,%u", aps ? "AP" : "STA", channel);
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
        /* A block bouncing across a bracketed field -- reads as a radar
         * scanning, even at zero found. Every frame holds the same glyphs
         * (one '#', three spaces), so the header width stays constant; the
         * proportional header font would otherwise make the line jitter as
         * the block moved. */
        static const char* const scan_frames[] = {
            "[#   ]", "[ #  ]", "[  # ]", "[   #]", "[  # ]", "[ #  ]"};
        const char* bar = scan_frames[s_spin % (sizeof(scan_frames) / sizeof(scan_frames[0]))];
        snprintf(
            header, sizeof(header), "Scan %s %u %s", bar, (unsigned)count,
            wh_scene_target_list_noun(app->search_kind));
    }
    submenu_set_header(app->submenu, header);
}

/* Append submenu rows for results heard since s_shown; returns the live count.
 * Never rebuilds existing rows, so the user's highlight stays where it is. */
static size_t wh_scene_target_list_sync_rows(WavehoundApp* app) {
    char label[64];
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    const size_t count = app->result_count;
    for(; s_shown < count; s_shown++) {
        format_label(&app->results[s_shown], label, sizeof(label));
        submenu_add_item(app->submenu, label, s_shown + 1, wh_scene_target_list_callback, app);
    }
    furi_mutex_release(app->results_mutex);
    return count;
}

/* Clear the list and (re)start a continuous scan. */
static void wh_scene_target_list_restart(WavehoundApp* app) {
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    app->result_count = 0;
    furi_mutex_release(app->results_mutex);
    s_shown = 0;
    s_header_count = 0;

    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu, "~ Search again", ITEM_SEARCH_AGAIN, wh_scene_target_list_callback, app);
    wh_scene_target_list_set_header(app, 0);

    if(app->uart) wh_scene_target_list_send_discover(app);
}

/* Re-enter after drilling into a result: keep every row found so far and just
 * rebuild the submenu that on_exit tore down, then resume the board scan. */
static void wh_scene_target_list_resume(WavehoundApp* app) {
    s_shown = 0;
    submenu_reset(app->submenu);
    submenu_add_item(
        app->submenu, "~ Search again", ITEM_SEARCH_AGAIN, wh_scene_target_list_callback, app);

    const size_t count = wh_scene_target_list_sync_rows(app);
    wh_scene_target_list_set_header(app, count);
    s_header_count = count;

    if(app->uart) wh_scene_target_list_send_discover(app);
}

void wh_scene_target_list_on_enter(void* context) {
    WavehoundApp* app = context;
    const bool resume =
        scene_manager_get_scene_state(app->scene_manager, WhSceneTargetList) ==
        TARGET_LIST_RESUME;
    if(resume) {
        wh_scene_target_list_resume(app);
        submenu_set_selected_item(app->submenu, s_picked_item);
    } else {
        wh_scene_target_list_restart(app);
        submenu_set_selected_item(app->submenu, ITEM_SEARCH_AGAIN);
    }
    /* The next entry is fresh unless a pick arms RESUME again. */
    scene_manager_set_scene_state(app->scene_manager, WhSceneTargetList, TARGET_LIST_FRESH);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_target_list_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeTick) {
        /* Append devices heard since the last tick; never rebuild, so the
         * highlight stays where the user left it. */
        const size_t count = wh_scene_target_list_sync_rows(app);
        /* While a live scan is on, refresh every tick so the sweep animates;
         * otherwise only rewrite the header when the count actually changes. */
        if(app->uart && !app->picking_ap_channel) {
            s_spin++;
            wh_scene_target_list_set_header(app, count);
        } else if(count != s_header_count) {
            wh_scene_target_list_set_header(app, count);
        }
        s_header_count = count;
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

    /* Keep the found list when we come back from Details, and land back on the
     * row just picked. */
    s_picked_item = event.event;
    scene_manager_set_scene_state(app->scene_manager, WhSceneTargetList, TARGET_LIST_RESUME);

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
