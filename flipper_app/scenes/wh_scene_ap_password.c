#include "../wavehound_i.h"
#include "../wh_ap_store.h"

#include <string.h>

/* Enter the Wi-Fi password for app->clients_ap, then connect. Reused from the
 * Saved APs menu in edit-only mode (app->ap_password_edit): it then rewrites the
 * stored psk for app->saved_aps[app->ap_edit_index] instead of connecting. */

static void wh_scene_ap_password_done(void* context) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WhEventPskEntered);
}

void wh_scene_ap_password_on_enter(void* context) {
    WavehoundApp* app = context;
    const bool edit = app->ap_password_edit && app->ap_edit_index < app->saved_ap_count;

    /* Editing prefills the current password so it can be tweaked; a fresh login
     * starts empty. */
    if(edit) {
        strlcpy(app->psk_buf, app->saved_aps[app->ap_edit_index].psk, sizeof(app->psk_buf));
    } else {
        app->psk_buf[0] = '\0';
    }

    text_input_reset(app->text_input);
    text_input_set_header_text(app->text_input, edit ? "New password" : "Wi-Fi password");
    text_input_set_minimum_length(app->text_input, 1);
    text_input_set_result_callback(
        app->text_input,
        wh_scene_ap_password_done,
        app,
        app->psk_buf,
        sizeof(app->psk_buf),
        !edit); /* clear the default only for a fresh login */
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewTextInput);
}

bool wh_scene_ap_password_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type != SceneManagerEventTypeCustom || event.event != WhEventPskEntered) {
        return false;
    }

    if(app->ap_password_edit) {
        /* Edit-only: rewrite the stored credential, don't touch the connection. */
        if(app->ap_edit_index < app->saved_ap_count) {
            const WhSavedAp* a = &app->saved_aps[app->ap_edit_index];
            wh_ap_store_add(app, a->ssid, a->security, app->psk_buf);
        }
        scene_manager_previous_scene(app->scene_manager); /* back to the entry menu */
        return true;
    }

    app->login_pending_save = true; /* a fresh password -> offer to save on success */
    wh_ap_connect(app);
    scene_manager_next_scene(app->scene_manager, WhSceneApConnect);
    return true;
}

void wh_scene_ap_password_on_exit(void* context) {
    WavehoundApp* app = context;
    /* Clear the edit flag so the next (login-flow) entry is a fresh connect. */
    app->ap_password_edit = false;
    text_input_reset(app->text_input);
}
