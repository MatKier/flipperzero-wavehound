#include "../wavehound_i.h"

#include <stdio.h>

/* Shows the result of a target fingerprint: the open TCP ports, the HTTP Server:
 * banner / page title, and the board's device-class guess. Reached from the
 * Details screen (center "Scan") and from the hunt screen (Up). on_enter kicks
 * the scan (FP,<mac> for app->target); the board streams FP,... lines that the
 * UART worker folds into app->fp and signals with WhEventFingerprintUpdate. */

static void wh_scene_fingerprint_render(WavehoundApp* app) {
    /* Snapshot the shared result so drawing holds the lock only briefly. */
    WhFingerprint fp;
    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    fp = app->fp;
    furi_mutex_release(app->results_mutex);

    Widget* w = app->widget;
    widget_reset(w);

    /* Headline: the board's guess, else the resolved host, else the IP. */
    const char* headline = fp.dtype[0]            ? fp.dtype :
                           app->target.host[0]    ? app->target.host :
                           fp.ip[0]               ? fp.ip :
                                                    app->target.mac;
    widget_add_string_element(w, 0, 11, AlignLeft, AlignBottom, FontPrimary, headline);

    char line[64];
    snprintf(line, sizeof(line), "IP: %s", fp.ip[0] ? fp.ip : "?");
    widget_add_string_element(w, 0, 23, AlignLeft, AlignBottom, FontSecondary, line);

    /* Open ports, or the scan's progress when there are none to show yet. */
    if(fp.port_count) {
        size_t n = snprintf(line, sizeof(line), "Ports:");
        for(uint8_t i = 0; i < fp.port_count && n < sizeof(line); i++) {
            n += snprintf(line + n, sizeof(line) - n, " %u", fp.ports[i]);
        }
    } else if(fp.running) {
        snprintf(line, sizeof(line), "Ports: scanning...");
    } else {
        snprintf(line, sizeof(line), "No open ports");
    }
    widget_add_string_element(w, 0, 34, AlignLeft, AlignBottom, FontSecondary, line);

    /* HTTP banner: the Server: header is the more telling of the two; fall back
     * to the page title. The widget clips a long value at the screen edge. */
    if(fp.server[0]) {
        snprintf(line, sizeof(line), "Srv: %s", fp.server);
        widget_add_string_element(w, 0, 45, AlignLeft, AlignBottom, FontSecondary, line);
    } else if(fp.title[0]) {
        snprintf(line, sizeof(line), "Web: %s", fp.title);
        widget_add_string_element(w, 0, 45, AlignLeft, AlignBottom, FontSecondary, line);
    }

    /* Footer status. */
    const char* footer = fp.err     ? "" :
                         fp.running  ? "Scanning..." :
                         fp.done     ? "Done" :
                                       "";
    if(fp.err) {
        snprintf(line, sizeof(line), "Failed: %s", fp.err_msg[0] ? fp.err_msg : "error");
        widget_add_string_element(w, 0, 62, AlignLeft, AlignBottom, FontSecondary, line);
    } else if(footer[0]) {
        widget_add_string_element(w, 127, 62, AlignRight, AlignBottom, FontSecondary, footer);
    }
}

void wh_scene_fingerprint_on_enter(void* context) {
    WavehoundApp* app = context;
    wh_fingerprint_start(app); /* resets app->fp and sends FP,<mac> */
    wh_scene_fingerprint_render(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewWidget);
}

bool wh_scene_fingerprint_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;
    if(event.type == SceneManagerEventTypeCustom && event.event == WhEventFingerprintUpdate) {
        wh_scene_fingerprint_render(app);
        return true;
    }
    return false;
}

void wh_scene_fingerprint_on_exit(void* context) {
    WavehoundApp* app = context;
    wh_send(app, "FP,STOP"); /* abort if the scan is still running */
    widget_reset(app->widget);
}
