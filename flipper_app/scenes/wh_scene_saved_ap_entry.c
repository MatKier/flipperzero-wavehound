#include "../wavehound_i.h"
#include "../wh_ap_store.h"

/* Actions for one saved AP, chosen from the Saved APs list. app->ap_edit_index
 * names the entry. */

#define ENTRY_EDIT_PASSWORD 0
#define ENTRY_DELETE 1

static void wh_scene_saved_ap_entry_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wh_scene_saved_ap_entry_on_enter(void* context) {
    WavehoundApp* app = context;
    if(app->ap_edit_index >= app->saved_ap_count) {
        scene_manager_previous_scene(app->scene_manager);
        return;
    }
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, app->saved_aps[app->ap_edit_index].ssid);
    submenu_add_item(
        submenu, "Edit password", ENTRY_EDIT_PASSWORD, wh_scene_saved_ap_entry_callback, app);
    submenu_add_item(submenu, "Delete", ENTRY_DELETE, wh_scene_saved_ap_entry_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_saved_ap_entry_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(app->ap_edit_index >= app->saved_ap_count) return false;

    switch(event.event) {
    case ENTRY_EDIT_PASSWORD:
        /* Reuse the password text-input in edit-only mode: it rewrites the
         * stored psk for this SSID instead of connecting. */
        app->ap_password_edit = true;
        scene_manager_next_scene(app->scene_manager, WhSceneApPassword);
        return true;
    case ENTRY_DELETE:
        wh_ap_store_delete(app, app->ap_edit_index);
        scene_manager_previous_scene(app->scene_manager); /* back to the list */
        return true;
    default:
        return false;
    }
}

void wh_scene_saved_ap_entry_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
