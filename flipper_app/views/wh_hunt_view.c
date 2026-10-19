#include "wh_hunt_view.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

#define HISTORY_LEN 128
#define LOST_AFTER_MS 2000
#define RSSI_FLOOR (-95)
#define RSSI_CEIL (-30)

#define GRAPH_TOP 44
#define GRAPH_BOTTOM 63

typedef enum {
    GeigerEvtStop = (1 << 0),
} GeigerEvt;

typedef struct {
    char name[40];
    uint8_t channel;
    bool locked;
    bool heard; /* heard the target within LOST_AFTER_MS */
    float rssi; /* smoothed, 0 = nothing yet */
    int8_t peak;
    uint32_t frames_total;
    int8_t history[HISTORY_LEN];
    uint8_t history_pos;
    bool sound;
} WhHuntModel;

struct WhHuntView {
    View* view;
    FuriThread* geiger;
    volatile int8_t geiger_rssi; /* 0 = silent */
    volatile bool sound;
    volatile float volume; /* click loudness, 0..1 */
    uint32_t last_heard_tick;
    WhHuntSaveCallback save_cb;
    void* save_ctx;
};

static float rssi_fraction(float rssi) {
    if(rssi == 0) return 0;
    float t = (rssi - RSSI_FLOOR) / (float)(RSSI_CEIL - RSSI_FLOOR);
    if(t < 0) t = 0;
    if(t > 1) t = 1;
    return t;
}

static const char* heat_word(const WhHuntModel* m) {
    if(!m->heard) return m->locked ? "LOST" : "SEEK";
    if(m->rssi >= -45) return "HOT!";
    if(m->rssi >= -60) return "WARM";
    if(m->rssi >= -75) return "COOL";
    return "COLD";
}

static void wh_hunt_draw(Canvas* canvas, void* model) {
    WhHuntModel* m = model;
    char buf[24];
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* Header: target + channel */
    canvas_set_font(canvas, FontSecondary);
    if(m->locked) {
        snprintf(buf, sizeof(buf), "ch%u", m->channel);
    } else {
        snprintf(buf, sizeof(buf), "?ch%u", m->channel);
    }
    canvas_draw_str_aligned(canvas, 128, 0, AlignRight, AlignTop, buf);
    canvas_set_font(canvas, FontPrimary);
    FuriString* name = furi_string_alloc_set(m->name);
    elements_string_fit_width(canvas, name, 104);
    canvas_draw_str(canvas, 0, 9, furi_string_get_cstr(name));
    furi_string_free(name);

    /* Big RSSI readout */
    if(m->heard) {
        int v = (int)(m->rssi - 0.5f);
        canvas_draw_box(canvas, 1, 22, 6, 3); /* minus sign: big font has no '-' */
        canvas_set_font(canvas, FontBigNumbers);
        snprintf(buf, sizeof(buf), "%d", -v);
        canvas_draw_str(canvas, 9, 30, buf);
        int digits_w = canvas_string_width(canvas, buf);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 9 + digits_w + 3, 30, "dBm");
    } else {
        canvas_draw_box(canvas, 1, 22, 6, 3);
        canvas_draw_box(canvas, 9, 22, 10, 3);
        canvas_draw_box(canvas, 22, 22, 10, 3);
    }

    /* Heat word + peak */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 127, 14, AlignRight, AlignTop, heat_word(m));
    canvas_set_font(canvas, FontSecondary);
    if(m->peak) {
        snprintf(buf, sizeof(buf), "pk %d", m->peak);
        canvas_draw_str_aligned(canvas, 127, 24, AlignRight, AlignTop, buf);
    }

    /* Meter */
    canvas_draw_rframe(canvas, 0, 34, 128, 8, 2);
    int fill = (int)(rssi_fraction(m->heard ? m->rssi : 0) * 124);
    if(fill > 0) canvas_draw_box(canvas, 2, 36, fill, 4);
    if(m->peak) {
        int px = 2 + (int)(rssi_fraction(m->peak) * 123);
        canvas_draw_line(canvas, px, 33, px, 42);
    }

    /* History graph, newest sample on the right */
    const int graph_h = GRAPH_BOTTOM - GRAPH_TOP;
    for(int i = 0; i < HISTORY_LEN; i++) {
        int8_t s = m->history[(m->history_pos + i) % HISTORY_LEN];
        if(s == 0) continue;
        int h = 1 + (int)(rssi_fraction(s) * graph_h);
        canvas_draw_line(canvas, i, GRAPH_BOTTOM, i, GRAPH_BOTTOM - h + 1);
    }
    canvas_draw_dot(canvas, 0, GRAPH_TOP);

    if(!m->sound) {
        canvas_set_font(canvas, FontKeyboard);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, GRAPH_TOP, 25, 9);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, 1, GRAPH_TOP + 8, "MUTE");
    }
}

static bool wh_hunt_input(InputEvent* event, void* context) {
    WhHuntView* hunt = context;
    if(event->type != InputTypeShort) return false;

    if(event->key == InputKeyOk) {
        with_view_model(
            hunt->view,
            WhHuntModel * m,
            {
                m->sound = !m->sound;
                hunt->sound = m->sound;
            },
            true);
        return true;
    }
    if(event->key == InputKeyLeft) {
        with_view_model(hunt->view, WhHuntModel * m, { m->peak = 0; }, true);
        return true;
    }
    if(event->key == InputKeyRight) {
        if(hunt->save_cb) hunt->save_cb(hunt->save_ctx);
        return true;
    }
    return false;
}

static float geiger_rate(int8_t rssi) {
    /* clicks per second: ~1 when barely audible, ~60 when on top of it */
    if(rssi == 0) return 0;
    float t = rssi_fraction(rssi);
    return 1.0f + t * t * 60.0f;
}

static int32_t wh_geiger_thread(void* context) {
    WhHuntView* hunt = context;
    bool have_speaker = false;
    uint8_t click_ticks = 0;

    for(;;) {
        uint32_t ev = furi_thread_flags_wait(GeigerEvtStop, FuriFlagWaitAny, furi_ms_to_ticks(10));
        if(!(ev & FuriFlagError) && (ev & GeigerEvtStop)) break;

        if(hunt->sound && !have_speaker) {
            have_speaker = furi_hal_speaker_acquire(30);
        } else if(!hunt->sound && have_speaker) {
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
            have_speaker = false;
        }
        if(!have_speaker) continue;

        if(click_ticks) {
            if(--click_ticks == 0) furi_hal_speaker_stop();
            continue;
        }

        /* Poisson-ish clicks: probability per 10 ms tick = rate / 100 */
        float rate = geiger_rate(hunt->geiger_rssi);
        uint32_t threshold = (uint32_t)(rate * 100.0f);
        uint32_t rnd = furi_hal_random_get();
        if(threshold && (rnd % 10000) < threshold) {
            furi_hal_speaker_start(900.0f + (float)((rnd >> 16) % 900), hunt->volume);
            click_ticks = 1;
        }
    }

    if(have_speaker) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
    }
    return 0;
}

static void wh_hunt_enter(void* context) {
    WhHuntView* hunt = context;
    hunt->geiger = furi_thread_alloc_ex("WhGeiger", 1024, wh_geiger_thread, hunt);
    furi_thread_start(hunt->geiger);
}

static void wh_hunt_exit(void* context) {
    WhHuntView* hunt = context;
    if(hunt->geiger) {
        furi_thread_flags_set(furi_thread_get_id(hunt->geiger), GeigerEvtStop);
        furi_thread_join(hunt->geiger);
        furi_thread_free(hunt->geiger);
        hunt->geiger = NULL;
    }
}

WhHuntView* wh_hunt_view_alloc(void) {
    WhHuntView* hunt = malloc(sizeof(WhHuntView));
    memset(hunt, 0, sizeof(WhHuntView));
    hunt->sound = true;
    hunt->volume = 0.5f;
    hunt->view = view_alloc();
    view_allocate_model(hunt->view, ViewModelTypeLocking, sizeof(WhHuntModel));
    view_set_context(hunt->view, hunt);
    view_set_draw_callback(hunt->view, wh_hunt_draw);
    view_set_input_callback(hunt->view, wh_hunt_input);
    view_set_enter_callback(hunt->view, wh_hunt_enter);
    view_set_exit_callback(hunt->view, wh_hunt_exit);
    with_view_model(
        hunt->view,
        WhHuntModel * m,
        {
            memset(m, 0, sizeof(WhHuntModel));
            m->sound = true;
        },
        false);
    return hunt;
}

void wh_hunt_view_free(WhHuntView* hunt) {
    view_free(hunt->view);
    free(hunt);
}

View* wh_hunt_view_get_view(WhHuntView* hunt) {
    return hunt->view;
}

void wh_hunt_view_set_save_callback(WhHuntView* hunt, WhHuntSaveCallback cb, void* context) {
    hunt->save_cb = cb;
    hunt->save_ctx = context;
}

void wh_hunt_view_set_volume(WhHuntView* hunt, float volume) {
    if(volume < 0) volume = 0;
    if(volume > 1.0f) volume = 1.0f;
    hunt->volume = volume;
}

void wh_hunt_view_set_sound(WhHuntView* hunt, bool on) {
    hunt->sound = on;
    with_view_model(hunt->view, WhHuntModel * m, { m->sound = on; }, true);
}

uint8_t wh_hunt_view_current_channel(WhHuntView* hunt) {
    uint8_t ch = 0;
    with_view_model(
        hunt->view, WhHuntModel * m, { ch = m->locked ? m->channel : 0; }, false);
    return ch;
}

void wh_hunt_view_set_target(WhHuntView* hunt, const char* name, uint8_t channel) {
    hunt->geiger_rssi = 0;
    hunt->last_heard_tick = 0;
    with_view_model(
        hunt->view,
        WhHuntModel * m,
        {
            bool sound = m->sound;
            memset(m, 0, sizeof(WhHuntModel));
            m->sound = sound;
            strlcpy(m->name, name, sizeof(m->name));
            m->channel = channel;
            m->locked = channel != 0;
        },
        true);
}

void wh_hunt_view_push_sample(
    WhHuntView* hunt,
    uint8_t channel,
    uint32_t frames,
    int8_t last,
    int8_t avg,
    int8_t max,
    bool locked) {
    UNUSED(last);
    uint32_t now = furi_get_tick();
    if(frames) hunt->last_heard_tick = now;
    bool heard = hunt->last_heard_tick &&
                 (now - hunt->last_heard_tick) < furi_ms_to_ticks(LOST_AFTER_MS);

    int8_t geiger = 0;
    with_view_model(
        hunt->view,
        WhHuntModel * m,
        {
            m->channel = channel;
            m->locked = locked;
            m->heard = heard;
            if(frames) {
                m->frames_total += frames;
                m->rssi = (m->rssi == 0) ? avg : (m->rssi * 0.6f + avg * 0.4f);
                if(m->peak == 0 || max > m->peak) m->peak = max;
            } else if(!heard) {
                m->rssi = 0;
            }
            m->history[m->history_pos] = frames ? avg : 0;
            m->history_pos = (m->history_pos + 1) % HISTORY_LEN;
            geiger = heard ? (int8_t)m->rssi : 0;
        },
        true);
    hunt->geiger_rssi = geiger;
}
