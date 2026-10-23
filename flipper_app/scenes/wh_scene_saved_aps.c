#include "../wavehound_i.h"

/* Saved AP credentials. Each entry opens an Edit password / Delete menu
 * (WhSceneSavedApEntry). Credentials are stored by the "Clients of nearby AP"
 * login flow (Enter password / WPS, then "Save"). */

static const char* const empty_text =
    "\e#No saved APs\n"
    "Log in to an AP from\n"
    "Hunt devices > Clients of\n"
    "nearby AP, then Save, to\n"
    "keep its password here.";

static void wh_scene_saved_aps_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wh_scene_saved_aps_on_enter(void* context) {
    WavehoundApp* app = context;
    if(app->saved_ap_count == 0) {
        widget_reset(app->widget);
        widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, empty_text);
        view_dispatcher_switch_to_view(app->view_dispatcher, WhViewWidget);
        return;
    }

    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(submenu, "Saved APs");
    for(size_t i = 0; i < app->saved_ap_count; i++) {
        submenu_add_item(
            submenu, app->saved_aps[i].ssid, (uint32_t)i, wh_scene_saved_aps_callback, app);
    }
    /* Keep the previously chosen row selected when coming back from an entry. */
    size_t selected = app->ap_edit_index < app->saved_ap_count ? app->ap_edit_index : 0;
    submenu_set_selected_item(submenu, selected);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_saved_aps_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    const size_t index = (size_t)event.event;
    if(index >= app->saved_ap_count) return false;

    app->ap_edit_index = index;
    scene_manager_next_scene(app->scene_manager, WhSceneSavedApEntry);
    return true;
}

void wh_scene_saved_aps_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
    widget_reset(app->widget);
}
