#include "../wavehound_i.h"
#include "../wh_ap_store.h"

#include <stdio.h>

/* Reached after picking an AP in the "Clients of nearby AP..." flow: offer to
 * log in to that AP (so a silent client can later be elicited) before listing
 * its clients. app->clients_ap holds the chosen AP. */

enum {
    ApLoginConnectSaved,
    ApLoginConnectOpen,
    ApLoginPassword,
    ApLoginWps,
    ApLoginSkip,
};

static void wh_scene_ap_login_cb(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wh_scene_ap_login_on_enter(void* context) {
    WavehoundApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);

    const WhTarget* ap = &app->clients_ap;
    char header[48];
    snprintf(header, sizeof(header), "Log in to %s?", ap->name[0] ? ap->name : ap->mac);
    submenu_set_header(submenu, header);

    if(strcmp(ap->security, "Open") == 0) {
        submenu_add_item(submenu, "Connect (open)", ApLoginConnectOpen, wh_scene_ap_login_cb, app);
    } else {
        if(wh_ap_store_find(app, ap->name)) {
            submenu_add_item(
                submenu, "Connect (saved)", ApLoginConnectSaved, wh_scene_ap_login_cb, app);
        }
        submenu_add_item(submenu, "Enter password", ApLoginPassword, wh_scene_ap_login_cb, app);
        submenu_add_item(submenu, "WPS (push button)", ApLoginWps, wh_scene_ap_login_cb, app);
    }
    submenu_add_item(submenu, "Skip (list only)", ApLoginSkip, wh_scene_ap_login_cb, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_ap_login_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case ApLoginConnectSaved: {
        const WhSavedAp* a = wh_ap_store_find(app, app->clients_ap.name);
        strlcpy(app->psk_buf, a ? a->psk : "", sizeof(app->psk_buf));
        app->login_pending_save = false; /* already stored */
        wh_ap_connect(app);
        scene_manager_next_scene(app->scene_manager, WhSceneApConnect);
        return true;
    }
    case ApLoginConnectOpen:
        app->psk_buf[0] = '\0';
        app->login_pending_save = false; /* nothing to save for an open network */
        wh_ap_connect(app);
        scene_manager_next_scene(app->scene_manager, WhSceneApConnect);
        return true;
    case ApLoginPassword:
        scene_manager_next_scene(app->scene_manager, WhSceneApPassword);
        return true;
    case ApLoginWps:
        scene_manager_next_scene(app->scene_manager, WhSceneApWps);
        return true;
    case ApLoginSkip:
        wh_ap_login_proceed(app);
        return true;
    default:
        return false;
    }
}

void wh_scene_ap_login_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
