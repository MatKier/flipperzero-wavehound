#include "../wavehound_i.h"

typedef enum {
    StartItemHuntAp,
    StartItemSearchDevices,
    StartItemEnterMac,
    StartItemSaved,
    StartItemSpectrum,
    StartItemBoardInfo,
    StartItemAbout,
} StartItem;

static bool s_header_alive;

static void wh_scene_start_update_header(WavehoundApp* app) {
    s_header_alive = app->board_alive;
    if(!app->uart) {
        submenu_set_header(app->submenu, "Wavehound - USART busy!");
    } else {
        submenu_set_header(
            app->submenu, app->board_alive ? "Wavehound - board OK" : "Wavehound - no board");
    }
}

static void wh_scene_start_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wh_scene_start_on_enter(void* context) {
    WavehoundApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_add_item(submenu, "Hunt access point", StartItemHuntAp, wh_scene_start_callback, app);
    submenu_add_item(
        submenu, "Hunt devices", StartItemSearchDevices, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Enter MAC", StartItemEnterMac, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Saved MACs", StartItemSaved, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Channel spectrum", StartItemSpectrum, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Board info", StartItemBoardInfo, wh_scene_start_callback, app);
    submenu_add_item(submenu, "About / Help", StartItemAbout, wh_scene_start_callback, app);
    wh_scene_start_update_header(app);
    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, WhSceneStart));
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_start_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type == SceneManagerEventTypeTick) {
        if(app->board_alive != s_header_alive) wh_scene_start_update_header(app);
        return false;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    scene_manager_set_scene_state(app->scene_manager, WhSceneStart, event.event);
    switch(event.event) {
    case StartItemHuntAp:
        app->search_kind = WhSearchAccessPoints;
        app->picking_ap_channel = false;
        scene_manager_next_scene(app->scene_manager, WhSceneListenTime);
        return true;
    case StartItemSearchDevices:
        app->channel_purpose = WhChannelForDeviceSearch;
        scene_manager_next_scene(app->scene_manager, WhSceneChannelSelect);
        return true;
    case StartItemEnterMac:
        app->editing = false; /* fresh manual entry, not editing a saved entry */
        scene_manager_next_scene(app->scene_manager, WhSceneMacInput);
        return true;
    case StartItemSaved:
        scene_manager_next_scene(app->scene_manager, WhSceneSaved);
        return true;
    case StartItemSpectrum:
        scene_manager_next_scene(app->scene_manager, WhSceneSpectrum);
        return true;
    case StartItemBoardInfo:
        scene_manager_next_scene(app->scene_manager, WhSceneBoardInfo);
        return true;
    case StartItemAbout:
        scene_manager_next_scene(app->scene_manager, WhSceneAbout);
        return true;
    default:
        return false;
    }
}

void wh_scene_start_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
