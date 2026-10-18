#include "../wavehound_i.h"

#define LISTEN_GRACE_MS 8000 /* time for the board to send its report */

static uint32_t s_deadline_tick; /* give up if no report by then */
static bool s_failed;
static bool s_finishing;

static void wh_scene_searching_finish_callback(void* context) {
    WavehoundApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, WhEventFinishEarly);
}

void wh_scene_searching_on_enter(void* context) {
    WavehoundApp* app = context;
    const WhSearchKind kind = app->search_kind;
    const bool aps = kind == WhSearchAccessPoints;
    const bool clients = kind == WhSearchClients;
    /* AP listens always hop; device listens may be parked on one channel;
     * client listens park on the chosen AP's channel. */
    const uint8_t channel = aps ? 0 : clients ? app->clients_ap.channel : app->device_channel;
    const uint16_t secs = app->listen_seconds;
    const char* noun = aps ? "Networks" : clients ? "Clients" : "Devices";

    char duration[8];
    if(secs >= 60) {
        snprintf(duration, sizeof(duration), "%um", secs / 60);
    } else {
        snprintf(duration, sizeof(duration), "%us", secs);
    }
    char title[24];
    if(channel) {
        snprintf(title, sizeof(title), "%s %s ch%u", noun, duration, channel);
    } else {
        snprintf(title, sizeof(title), "%s %s", noun, duration);
    }
    char cmd[48];
    if(clients) {
        snprintf(
            cmd, sizeof(cmd), "DISCOVER,%u,CLIENTS,%u,%s", secs, channel, app->clients_ap.mac);
    } else {
        snprintf(cmd, sizeof(cmd), "DISCOVER,%u,%s,%u", secs, aps ? "AP" : "STA", channel);
    }

    furi_mutex_acquire(app->results_mutex, FuriWaitForever);
    app->result_count = 0;
    app->result_total = 0;
    app->progress_found = 0;
    furi_mutex_release(app->results_mutex);

    s_deadline_tick = furi_get_tick() + furi_ms_to_ticks(secs * 1000 + LISTEN_GRACE_MS);
    s_failed = false;
    s_finishing = false;
    wh_progress_view_start(app->progress, title, secs * 1000);
    wh_progress_view_set_finish_callback(app->progress, wh_scene_searching_finish_callback, app);
    if(app->uart) {
        wh_send(app, cmd);
    } else {
        s_failed = true;
        wh_progress_view_set_error(app->progress, "USART busy");
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, WhViewProgress);
}

bool wh_scene_searching_on_event(void* context, SceneManagerEvent event) {
    WavehoundApp* app = context;

    if(event.type == SceneManagerEventTypeCustom && event.event == WhEventResultsReady) {
        if(s_failed) return true;
        scene_manager_set_scene_state(app->scene_manager, WhSceneTargetList, 0);
        scene_manager_next_scene(app->scene_manager, WhSceneTargetList);
        return true;
    }

    if(event.type == SceneManagerEventTypeCustom && event.event == WhEventFinishEarly) {
        if(s_failed || s_finishing) return true;
        /* Board stops listening and sends what it has right away. */
        s_finishing = true;
        s_deadline_tick = furi_get_tick() + furi_ms_to_ticks(LISTEN_GRACE_MS);
        wh_send(app, "FINISH");
        wh_progress_view_set_finishing(app->progress);
        return true;
    }

    if(event.type == SceneManagerEventTypeTick && !s_failed) {
        wh_progress_view_update(app->progress, app->progress_found);
        if((int32_t)(furi_get_tick() - s_deadline_tick) > 0) {
            s_failed = true;
            wh_send(app, "STOP");
            wh_progress_view_set_error(app->progress, "Board not responding");
        }
    }
    return false;
}

void wh_scene_searching_on_exit(void* context) {
    WavehoundApp* app = context;
    wh_progress_view_set_finish_callback(app->progress, NULL, NULL);
    /* Cancels a running search; harmless once the board is idle. */
    wh_send(app, "STOP");
}
