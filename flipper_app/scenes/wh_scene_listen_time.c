#include "../wavehound_i.h"

/* Submenu index = seconds to listen. */
static const uint16_t listen_options[] = {5, 10, 30, 60, 120, 300};

static void wh_scene_listen_time_callback(void* context, uint32_t index) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void wh_scene_listen_time_on_enter(void* context) {
    WavehoundApp* app = context;
    Submenu* submenu = app->submenu;
    submenu_reset(submenu);
    submenu_set_header(
        submenu,
        app->search_kind == WhSearchAccessPoints ? "Listen for networks" :
        app->search_kind == WhSearchClients      ? "Listen for clients" :
                                                    "Listen for devices");

    char label[24];
    for(size_t i = 0; i < COUNT_OF(listen_options); i++) {
        const uint16_t secs = listen_options[i];
        if(secs < 60) {
            snprintf(label, sizeof(label), "%u seconds", secs);
        } else {
            snprintf(label, sizeof(label), "%u minute%s", secs / 60, secs == 60 ? "" : "s");
        }
        submenu_add_item(submenu, label, listen_options[i], wh_scene_listen_time_callback, app);
    }
    submenu_set_selected_item(submenu, app->listen_seconds);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewSubmenu);
}

bool wh_scene_listen_time_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    app->listen_seconds = (uint16_t)event.event;
    scene_manager_next_scene(app->scene_manager, WhSceneSearching);
    return true;
}

void wh_scene_listen_time_on_exit(void* context) {
    WavehoundApp* app = context;
    submenu_reset(app->submenu);
}
