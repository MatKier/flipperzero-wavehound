#include "../wavehound_i.h"

typedef enum {
    StartItemHuntAp,
    StartItemSearchDevices,
    StartItemEnterMac,
    StartItemSaved,
    StartItemSavedAps,
    StartItemSpectrum,
    StartItemBoardInfo,
    StartItemSettings,
    StartItemAbout,
} StartItem;

static bool s_header_alive;
static bool s_relit; /* relit the idle LED once the board came up this Start visit */

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
    s_relit = false;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_add_item(submenu, "Hunt access point", StartItemHuntAp, wh_scene_start_callback, app);
    submenu_add_item(
        submenu, "Hunt devices", StartItemSearchDevices, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Enter MAC", StartItemEnterMac, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Saved MACs", StartItemSaved, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Saved APs", StartItemSavedAps, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Channel spectrum", StartItemSpectrum, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Board info", StartItemBoardInfo, wh_scene_start_callback, app);
    submenu_add_item(submenu, "Settings", StartItemSettings, wh_scene_start_callback, app);
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
        /* The board idles dark after the SLEEP we send on exit, and the startup
         * STOP can be lost before the link settles. Relight the idle LED once the
         * board is confirmed alive (the menu is always a safe place to STOP). */
        if(app->board_alive && !s_relit) {
            wh_send(app, "STOP");
            s_relit = true;
        }
        return false;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    scene_manager_set_scene_state(app->scene_manager, WhSceneStart, event.event);
    switch(event.event) {
    case StartItemHuntAp:
        wh_conn_drop(app);
        app->search_kind = WhSearchAccessPoints;
        app->picking_ap_channel = false;
        scene_manager_next_scene(app->scene_manager, WhSceneTargetList);
        return true;
    case StartItemSearchDevices:
        wh_conn_drop(app);
        app->channel_purpose = WhChannelForDeviceSearch;
        scene_manager_next_scene(app->scene_manager, WhSceneChannelSelect);
        return true;
    case StartItemEnterMac:
        wh_conn_drop(app);
        app->editing = false; /* fresh manual entry, not editing a saved entry */
        scene_manager_next_scene(app->scene_manager, WhSceneMacInput);
        return true;
    case StartItemSaved:
        wh_conn_drop(app);
        scene_manager_next_scene(app->scene_manager, WhSceneSaved);
        return true;
    case StartItemSavedAps:
        /* Pure credential management (no scan), so the link may stay up. */
        scene_manager_next_scene(app->scene_manager, WhSceneSavedAps);
        return true;
    case StartItemSpectrum:
        wh_conn_drop(app);
        scene_manager_next_scene(app->scene_manager, WhSceneSpectrum);
        return true;
    case StartItemBoardInfo:
        scene_manager_next_scene(app->scene_manager, WhSceneBoardInfo);
        return true;
    case StartItemSettings:
        scene_manager_next_scene(app->scene_manager, WhSceneSettings);
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
