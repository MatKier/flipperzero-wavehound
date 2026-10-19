#include "../wavehound_i.h"

/* Submenu indexes: 0 = all/auto, 1..13 = that channel. */
#define ITEM_ALL_CHANNELS 0
#define ITEM_FROM_AP 100

static void wh_scene_channel_select_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

/* Continue to whatever needed the channel. */
static void wh_scene_channel_select_proceed(WavehoundApp* app) {
    if(app->channel_purpose == WhChannelForDeviceSearch) {
        /* A nearby AP was picked -> scope to its clients; a plain channel -> all
         * devices on it. */
        app->search_kind = app->device_search_clients ? WhSearchClients : WhSearchDevices;
        scene_manager_next_scene(app->scene_manager, WhSceneTargetList);
    } else {
        scene_manager_next_scene(app->scene_manager, WhSceneHunt);
    }
}

void wh_scene_channel_select_on_enter(void* context) {
    WavehoundApp* app = context;
    Submenu* submenu = app->submenu;
    const bool device_search = app->channel_purpose == WhChannelForDeviceSearch;
    submenu_reset(submenu);
    submenu_set_header(submenu, device_search ? "Device search channel" : "Hunt channel");

    submenu_add_item(
        submenu,
        device_search ? "All channels (hop)" : "Auto (hop until heard)",
        ITEM_ALL_CHANNELS,
        wh_scene_channel_select_callback,
        app);
    submenu_add_item(
        submenu,
        device_search ? "Clients of nearby AP..." : "Channel of nearby AP...",
        ITEM_FROM_AP,
        wh_scene_channel_select_callback,
        app);
    char label[16];
    for(uint32_t ch = 1; ch <= WH_MAX_CHANNEL; ch++) {
        snprintf(label, sizeof(label), "Channel %lu", (unsigned long)ch);
        submenu_add_item(submenu, label, ch, wh_scene_channel_select_callback, app);
    }

    uint32_t selected = device_search ? app->device_channel : app->target.channel;
    if(app->channel_pick_pending) {
        /* Back from the AP list with a channel: continue on the next loop turn. */
        selected = ITEM_FROM_AP;
        view_dispatcher_send_custom_event(app->view_dispatcher, WhEventChannelPicked);
    } else {
        app->picking_ap_channel = false;
    }
    submenu_set_selected_item(submenu, selected);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_channel_select_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == WhEventChannelPicked) {
        if(!app->channel_pick_pending) return true;
        app->channel_pick_pending = false;
        wh_scene_channel_select_proceed(app);
        return true;
    }

    if(event.event == ITEM_FROM_AP) {
        app->picking_ap_channel = true;
        app->search_kind = WhSearchAccessPoints;
        scene_manager_next_scene(app->scene_manager, WhSceneTargetList);
        return true;
    }

    if(event.event > WH_MAX_CHANNEL) return false;
    const uint8_t ch = (uint8_t)event.event; /* 0 = all / auto */
    if(app->channel_purpose == WhChannelForDeviceSearch) {
        app->device_channel = ch;
        app->device_search_clients = false; /* a plain channel: all devices, not scoped */
    } else {
        app->target.channel = ch;
        app->target.pin_channel = ch != 0;
    }
    wh_scene_channel_select_proceed(app);
    return true;
}

void wh_scene_channel_select_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
