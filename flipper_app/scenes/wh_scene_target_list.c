#include "../wavehound_i.h"

#define ITEM_SEARCH_AGAIN 0
/* Result i is submenu index i + 1. */

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

void wh_scene_target_list_on_enter(void* context) {
    WavehoundApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);

    char header[40];
    char label[64];

    const WhSearchKind kind = app->search_kind;
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    const size_t count = app->result_count;
    if(count == 0) {
        strlcpy(
            header,
            kind == WhSearchAccessPoints ? "No networks found" :
            kind == WhSearchClients      ? "No clients found" :
                                           "Nothing heard",
            sizeof(header));
    } else if(app->picking_ap_channel) {
        strlcpy(
            header,
            app->channel_purpose == WhChannelForDeviceSearch ? "Clients of which AP:" :
                                                               "Use channel of:",
            sizeof(header));
    } else {
        const char* noun = kind == WhSearchAccessPoints ? "networks" :
                           kind == WhSearchClients      ? "clients" :
                                                          "devices";
        if(app->result_total > count) {
            snprintf(
                header, sizeof(header), "%u %s, top %u", app->result_total, noun, (unsigned)count);
        } else {
            snprintf(header, sizeof(header), "%u %s", (unsigned)count, noun);
        }
    }
    submenu_set_header(submenu, header);
    submenu_add_item(
        submenu, "~ Search again", ITEM_SEARCH_AGAIN, wh_scene_target_list_callback, app);

    for(size_t i = 0; i < count; i++) {
        format_label(&app->results[i], label, sizeof(label));
        submenu_add_item(submenu, label, i + 1, wh_scene_target_list_callback, app);
    }
    furi_mutex_release(app->results_mutex);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, WhSceneTargetList));
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_target_list_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        /* Skip the finished Searching scene. The clients search kind stays set,
         * so re-picking a listen time scopes to the same AP. */
        static const uint32_t targets[] = {WhSceneListenTime, WhSceneStart};
        scene_manager_search_and_switch_to_previous_scene_one_of(
            app->scene_manager, targets, COUNT_OF(targets));
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == ITEM_SEARCH_AGAIN) {
        /* Searching is right below us on the stack; re-entering it restarts the search. */
        scene_manager_previous_scene(app->scene_manager);
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
             * it, on its channel. Searching reads clients_ap. */
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
    scene_manager_set_scene_state(app->scene_manager, WhSceneTargetList, event.event);
    scene_manager_next_scene(app->scene_manager, WhSceneDetails);
    return true;
}

void wh_scene_target_list_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
