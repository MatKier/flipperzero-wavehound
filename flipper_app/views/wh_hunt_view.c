#include "wh_hunt_view.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/elements.h>
#include <stdio.h>
#include <string.h>

#define HISTORY_LEN 128
#define LOST_AFTER_MS 2000
#define RSSI_FLOOR_DEFAULT (-95)
#define RSSI_CEIL_DEFAULT (-30)
#define RESPONSE_DEFAULT 0.4f

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
    float rssi_slow; /* slower EMA; fast-vs-slow gives the trend */
    int8_t trend; /* -1 falling, 0 steady, +1 rising */
    float fps; /* smoothed frames/second from the target */
    int8_t peak;
    int8_t nf; /* mean noise floor (dBm), 0 = unknown */
    uint16_t quiet_s; /* seconds since last heard, while lost (0 = heard/never) */
    uint32_t frames_total;
    int8_t history[HISTORY_LEN];
    uint8_t history_pos;
    bool sound;
    int8_t rssi_floor; /* dBm where the meter reads empty */
    int8_t rssi_ceil;  /* dBm where the meter reads full */
    char ip[16];       /* target IPv4 (shown as the label), else empty */
    char host[33];     /* target hostname (preferred label), else empty */
    char dtype[16];    /* target device class (label when there is no hostname), else empty */
    bool elicit_avail; /* elicitation offered (connected + IP known) */
    bool elicit_on;    /* elicitation currently running */
} WhHuntModel;

struct WhHuntView {
    View* view;
    FuriThread* geiger;
    volatile int8_t geiger_rssi; /* 0 = silent */
    volatile bool sound;
    volatile bool haptic; /* buzz the motor as a proximity cue */
    volatile float volume; /* click loudness, 0..1 */
    volatile int8_t rssi_floor; /* meter/click range, mirrors the model */
    volatile int8_t rssi_ceil;
    float response; /* weight of a new sample in the meter's smoothing */
    uint32_t last_heard_tick;
    WhHuntSaveCallback save_cb;
    void* save_ctx;
    WhHuntElicitCallback elicit_cb;
    void* elicit_ctx;
};

static float rssi_fraction(float rssi, int8_t floor, int8_t ceil) {
    if(rssi == 0) return 0;
    if(ceil <= floor) return 0;
    float t = (rssi - floor) / (float)(ceil - floor);
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
    /* Prefer a resolved hostname, then the device class, then the IP, falling
     * back to the SSID/MAC. */
    const char* label = m->host[0]  ? m->host :
                        m->dtype[0] ? m->dtype :
                        m->ip[0]    ? m->ip :
                                      m->name;
    FuriString* name = furi_string_alloc_set(label);
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
        /* Frame rate from the target: chatty AP vs. near-idle station. */
        if(m->fps >= 0.5f) {
            int dbm_w = canvas_string_width(canvas, "dBm");
            snprintf(buf, sizeof(buf), "%d/s", (int)(m->fps + 0.5f));
            canvas_draw_str(canvas, 9 + digits_w + 3 + dbm_w + 5, 30, buf);
        }
    } else {
        canvas_draw_box(canvas, 1, 22, 6, 3);
        canvas_draw_box(canvas, 9, 22, 10, 3);
        canvas_draw_box(canvas, 22, 22, 10, 3);
        /* How long the target has been silent, once it has been heard. */
        if(m->quiet_s) {
            canvas_set_font(canvas, FontSecondary);
            snprintf(buf, sizeof(buf), "quiet %us", m->quiet_s);
            canvas_draw_str(canvas, 40, 30, buf);
        }
    }

    /* Heat word + peak */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 127, 14, AlignRight, AlignTop, heat_word(m));
    /* Trend chevron left of the heat word: are we getting warmer or colder? */
    if(m->heard && m->trend != 0) {
        int ax = 127 - canvas_string_width(canvas, heat_word(m)) - 5;
        if(m->trend > 0) {
            canvas_draw_line(canvas, ax, 16, ax - 3, 20);
            canvas_draw_line(canvas, ax, 16, ax + 3, 20);
        } else {
            canvas_draw_line(canvas, ax, 20, ax - 3, 16);
            canvas_draw_line(canvas, ax, 20, ax + 3, 16);
        }
    }
    canvas_set_font(canvas, FontSecondary);
    if(m->peak) {
        snprintf(buf, sizeof(buf), "pk %d", m->peak);
        canvas_draw_str_aligned(canvas, 127, 24, AlignRight, AlignTop, buf);
    }

    /* Meter */
    canvas_draw_rframe(canvas, 0, 34, 128, 8, 2);
    int fill = (int)(rssi_fraction(m->heard ? m->rssi : 0, m->rssi_floor, m->rssi_ceil) * 124);
    if(fill > 0) canvas_draw_box(canvas, 2, 36, fill, 4);
    if(m->peak) {
        int px = 2 + (int)(rssi_fraction(m->peak, m->rssi_floor, m->rssi_ceil) * 123);
        canvas_draw_line(canvas, px, 33, px, 42);
    }
    /* Noise floor marker: a short tick above the meter. The gap to the signal
     * fill is the headroom. Distinct from the full-height peak tick. */
    if(m->nf) {
        int nx = 2 + (int)(rssi_fraction(m->nf, m->rssi_floor, m->rssi_ceil) * 123);
        canvas_draw_line(canvas, nx, 31, nx, 33); /* pokes above the meter */
        canvas_draw_line(canvas, nx, 42, nx, 43); /* and slightly below, like pk */
    }

    /* History graph, newest sample on the right */
    const int graph_h = GRAPH_BOTTOM - GRAPH_TOP;
    for(int i = 0; i < HISTORY_LEN; i++) {
        int8_t s = m->history[(m->history_pos + i) % HISTORY_LEN];
        if(s == 0) continue;
        int h = 1 + (int)(rssi_fraction(s, m->rssi_floor, m->rssi_ceil) * graph_h);
        canvas_draw_line(canvas, i, GRAPH_BOTTOM, i, GRAPH_BOTTOM - h + 1);
    }
    canvas_draw_dot(canvas, 0, GRAPH_TOP);

    /* Noise floor value, over the graph where the MUTE label used to sit. The
     * white box keeps it legible against the graph bars. */
    if(m->nf) {
        snprintf(buf, sizeof(buf), "nf %d", m->nf);
        canvas_set_font(canvas, FontSecondary);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, GRAPH_TOP, 40, 9);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_str(canvas, 1, GRAPH_TOP + 8, buf);
    }

    /* Elicitation (ping) status, bottom-right: plain "ping" when Down would arm
     * it, inverted when it is running. Both sit on a white backing so they stay
     * legible over the graph. */
    if(m->elicit_avail) {
        canvas_set_font(canvas, FontSecondary);
        const int tw = canvas_string_width(canvas, "ping");
        const int bx = 128 - tw - 4;
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, bx, GRAPH_TOP, tw + 4, 9);
        canvas_set_color(canvas, ColorBlack);
        if(m->elicit_on) {
            canvas_draw_box(canvas, bx, GRAPH_TOP, tw + 4, 9);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str(canvas, bx + 2, GRAPH_TOP + 8, "ping");
        canvas_set_color(canvas, ColorBlack);
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
    if(event->key == InputKeyDown) {
        bool avail = false;
        with_view_model(hunt->view, WhHuntModel * m, { avail = m->elicit_avail; }, false);
        if(avail && hunt->elicit_cb) {
            hunt->elicit_cb(hunt->elicit_ctx);
            return true;
        }
        return false;
    }
    return false;
}

static float geiger_rate(int8_t rssi, int8_t floor, int8_t ceil) {
    /* clicks per second: ~1 when barely audible, ~60 when on top of it */
    if(rssi == 0) return 0;
    float t = rssi_fraction(rssi, floor, ceil);
    return 1.0f + t * t * 60.0f;
}

/* Gap between haptic pulses in ms: slow (~900 ms) when barely heard, tightening
 * to a near-continuous buzz (~80 ms) on top of the target. 0 = stay silent. */
static uint32_t haptic_interval_ms(int8_t rssi, int8_t floor, int8_t ceil) {
    if(rssi == 0) return 0;
    float t = rssi_fraction(rssi, floor, ceil);
    return (uint32_t)(900.0f - t * 820.0f);
}

static int32_t wh_geiger_thread(void* context) {
    WhHuntView* hunt = context;
    bool have_speaker = false;
    uint8_t click_ticks = 0;
    bool vibro_on = false;
    uint8_t vibro_ticks = 0; /* remaining 10 ms ticks of the current pulse */
    uint32_t vibro_wait = 0; /* 10 ms ticks until the next pulse */

    for(;;) {
        uint32_t ev = furi_thread_flags_wait(GeigerEvtStop, FuriFlagWaitAny, furi_ms_to_ticks(10));
        if(!(ev & FuriFlagError) && (ev & GeigerEvtStop)) break;

        /* Haptic runs independently of sound, so handle it before the speaker
         * block (which skips the rest of the loop while muted). */
        if(hunt->haptic && hunt->geiger_rssi != 0) {
            if(vibro_ticks) {
                if(--vibro_ticks == 0) {
                    furi_hal_vibro_on(false);
                    vibro_on = false;
                }
            } else if(vibro_wait) {
                vibro_wait--;
            } else {
                furi_hal_vibro_on(true);
                vibro_on = true;
                vibro_ticks = 2; /* ~20 ms pulse */
                vibro_wait =
                    haptic_interval_ms(hunt->geiger_rssi, hunt->rssi_floor, hunt->rssi_ceil) / 10;
            }
        } else if(vibro_on || vibro_ticks) {
            furi_hal_vibro_on(false);
            vibro_on = false;
            vibro_ticks = 0;
            vibro_wait = 0;
        }

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
        float rate = geiger_rate(hunt->geiger_rssi, hunt->rssi_floor, hunt->rssi_ceil);
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
    if(vibro_on) furi_hal_vibro_on(false);
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
    hunt->response = RESPONSE_DEFAULT;
    hunt->rssi_floor = RSSI_FLOOR_DEFAULT;
    hunt->rssi_ceil = RSSI_CEIL_DEFAULT;
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
            m->rssi_floor = RSSI_FLOOR_DEFAULT;
            m->rssi_ceil = RSSI_CEIL_DEFAULT;
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

void wh_hunt_view_set_elicit_callback(WhHuntView* hunt, WhHuntElicitCallback cb, void* context) {
    hunt->elicit_cb = cb;
    hunt->elicit_ctx = context;
}

void wh_hunt_view_set_ip(
    WhHuntView* hunt,
    const char* ip,
    const char* host,
    const char* dtype) {
    with_view_model(
        hunt->view,
        WhHuntModel * m,
        {
            strlcpy(m->ip, ip ? ip : "", sizeof(m->ip));
            strlcpy(m->host, host ? host : "", sizeof(m->host));
            strlcpy(m->dtype, dtype ? dtype : "", sizeof(m->dtype));
        },
        true);
}

void wh_hunt_view_set_elicit_available(WhHuntView* hunt, bool available) {
    with_view_model(hunt->view, WhHuntModel * m, { m->elicit_avail = available; }, true);
}

void wh_hunt_view_set_elicit_on(WhHuntView* hunt, bool on) {
    with_view_model(hunt->view, WhHuntModel * m, { m->elicit_on = on; }, true);
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

void wh_hunt_view_set_haptic(WhHuntView* hunt, bool on) {
    hunt->haptic = on;
}

void wh_hunt_view_set_range(WhHuntView* hunt, int8_t floor, int8_t ceil) {
    hunt->rssi_floor = floor;
    hunt->rssi_ceil = ceil;
    with_view_model(
        hunt->view,
        WhHuntModel * m,
        {
            m->rssi_floor = floor;
            m->rssi_ceil = ceil;
        },
        true);
}

void wh_hunt_view_set_response(WhHuntView* hunt, float alpha) {
    if(alpha < 0.05f) alpha = 0.05f;
    if(alpha > 1.0f) alpha = 1.0f;
    hunt->response = alpha;
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
            int8_t floor = m->rssi_floor;
            int8_t ceil = m->rssi_ceil;
            memset(m, 0, sizeof(WhHuntModel));
            m->sound = sound;
            m->rssi_floor = floor;
            m->rssi_ceil = ceil;
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
    bool locked,
    int8_t nf) {
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
                m->rssi = (m->rssi == 0) ? avg :
                                            (m->rssi * (1.0f - hunt->response) + avg * hunt->response);
                m->rssi_slow = (m->rssi_slow == 0) ? avg : (m->rssi_slow * 0.96f + avg * 0.04f);
                float d = m->rssi - m->rssi_slow;
                /* Hysteresis so the arrow sticks instead of flickering: commit to a
                 * direction at +/-2 dB, and only clear it once the signal is
                 * genuinely flat (< 0.7 dB). The dead-band in between holds the
                 * last direction. */
                if(d > 2.0f)
                    m->trend = 1;
                else if(d < -2.0f)
                    m->trend = -1;
                else if(d > -0.7f && d < 0.7f)
                    m->trend = 0;
                m->fps = (m->fps == 0) ? frames * 10.0f : (m->fps * 0.7f + frames * 10.0f * 0.3f);
                if(m->peak == 0 || max > m->peak) m->peak = max;
                if(nf) m->nf = nf;
            } else if(heard) {
                m->fps *= 0.7f; /* still within the lost window: decay the activity */
            } else {
                m->rssi = 0;
                m->rssi_slow = 0;
                m->trend = 0;
                m->fps = 0;
            }
            m->quiet_s = (hunt->last_heard_tick && !heard) ?
                             (uint16_t)((now - hunt->last_heard_tick) / furi_ms_to_ticks(1000)) :
                             0;
            m->history[m->history_pos] = frames ? avg : 0;
            m->history_pos = (m->history_pos + 1) % HISTORY_LEN;
            geiger = heard ? (int8_t)m->rssi : 0;
        },
        true);
    hunt->geiger_rssi = geiger;
}
