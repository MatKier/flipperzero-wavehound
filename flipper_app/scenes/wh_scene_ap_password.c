#include "../wavehound_i.h"

/* Enter the Wi-Fi password for app->clients_ap, then connect. */

static void wh_scene_ap_password_done(void* context) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WhEventPskEntered);
}

void wh_scene_ap_password_on_enter(void* context) {
    WavehoundApp* app = context;
    app->psk_buf[0] = '\0';
    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, "Wi-Fi password");
    text_input_set_minimum_length(app->text_input, 1);
    text_input_set_result_callback(
        app->text_input,
        wh_scene_ap_password_done,
        app,
        app->psk_buf,
        sizeof(app->psk_buf),
        true); /* start empty */
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewTextInput);
}

bool wh_scene_ap_password_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != WhEventPskEntered) {
        return false;
    }
    app->login_pending_save = true; /* a fresh password -> offer to save on success */
    wh_ap_connect(app);
    scene_manager_next_scene(app->scene_manager, WhSceneApConnect);
    return true;
}

void wh_scene_ap_password_on_exit(void* context) {
    WavehoundApp* app = context;
    text_input_reset(app->text_input);
}
