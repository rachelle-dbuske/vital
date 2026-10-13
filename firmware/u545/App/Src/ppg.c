#include "ppg.h"

#include <string.h>

/* ---------- Tuning ---------- */
#define DC_TIME_CONSTANT_S     1.5f     /* DC tracking: slow average */
#define LP_ALPHA               0.3f     /* noise smoothing (~5 Hz cutoff at 100 Hz) */
#define FINGER_MIN_DC          20000.0f /* IR level below this = no finger */
#define DC_JUMP_FRACTION       0.25f    /* sudden change this large = re-acquire */
#define BEAT_THRESHOLD         0.5f     /* fraction of envelope that starts a beat */
#define ENV_DECAY_S            3.0f     /* envelope decay time constant */
#define MIN_BEAT_INTERVAL_S    0.20f    /* 300 BPM: faster beats are rejected */
#define MAX_BEAT_INTERVAL_S    2.50f    /* 24 BPM: slower means signal lost */
#define MIN_PERFUSION          0.001f   /* 0.1%: weaker pulse = invalid (REQ-PRC-04) */
#define INTERVAL_TOLERANCE     0.25f    /* beat-to-beat interval spread allowed */
#define HR_MIN_BPM             30.0f    /* REQ-PRC-01 */
#define HR_MAX_BPM             240.0f
#define SPO2_ACCURATE_MIN      70.0f    /* REQ-PRC-02: documentation only */
#define SPO2_MAX               100.0f

static float absf(float x) { return (x < 0.0f) ? -x : x; }

/* Forget all beat history (finger removed, signal lost, or level jump). */
static void reset_beats(ppg_t *p)
{
    p->env            = 0.0f;
    p->in_beat        = false;
    p->have_last_beat = false;
    p->hist_count     = 0U;
    p->hist_idx       = 0U;
    p->result.valid   = false;
}

static void reset_extremes(ppg_t *p)
{
    p->ir_max  = p->ir_lp;   p->ir_min  = p->ir_lp;
    p->red_max = p->red_lp;  p->red_min = p->red_lp;
}

void ppg_init(ppg_t *p, uint32_t fs)
{
    (void)memset(p, 0, sizeof(*p));
    p->fs = fs;
}

const ppg_result_t *ppg_result(const ppg_t *p)
{
    return &p->result;
}

/* Recompute HR, SpO2 and validity from the beat history. */
static void update_result(ppg_t *p)
{
    const uint32_t count = p->hist_count;
    float mean_interval  = 0.0f;
    float mean_ratio     = 0.0f;

    for (uint32_t i = 0U; i < count; i++) {
        mean_interval += (float)p->intervals[i];
        mean_ratio    += p->ratios[i];
    }
    mean_interval /= (float)count;
    mean_ratio    /= (float)count;

    /* Beats must be regular: every interval close to the mean. */
    bool regular = true;
    for (uint32_t i = 0U; i < count; i++) {
        if (absf((float)p->intervals[i] - mean_interval) > (INTERVAL_TOLERANCE * mean_interval)) {
            regular = false;
        }
    }

    /* Clamp to 0-100%. A very low value is NOT marked invalid: it is exactly
     * the reading the low-SpO2 alarm must see. Accuracy is specified only
     * over SPO2_ACCURATE_MIN..100% (REQ-PRC-02). */
    float spo2 = 110.0f - (25.0f * mean_ratio);
    if (spo2 > SPO2_MAX) {
        spo2 = SPO2_MAX;
    }
    if (spo2 < 0.0f) {
        spo2 = 0.0f;
    }

    p->result.hr_bpm = (60.0f * (float)p->fs) / mean_interval;
    p->result.spo2   = spo2;
    p->result.valid  = (count == PPG_HISTORY) && regular &&
                       (p->result.perfusion >= MIN_PERFUSION) &&
                       (p->result.hr_bpm >= HR_MIN_BPM) && (p->result.hr_bpm <= HR_MAX_BPM);
}

/* A beat has ended: record its interval and red/IR ratio. */
static void on_beat(ppg_t *p, uint32_t beat_n)
{
    const float ir_ac  = p->ir_max  - p->ir_min;
    const float red_ac = p->red_max - p->red_min;

    p->result.perfusion = ir_ac / p->ir_dc;
    p->result.beat_count++;

    if (p->have_last_beat && (ir_ac > 0.0f)) {
        const uint32_t interval = beat_n - p->last_beat_n;
        const float    ratio    = (red_ac / p->red_dc) / (ir_ac / p->ir_dc);

        p->intervals[p->hist_idx] = interval;
        p->ratios[p->hist_idx]    = ratio;
        p->hist_idx = (p->hist_idx + 1U) % PPG_HISTORY;
        if (p->hist_count < PPG_HISTORY) {
            p->hist_count++;
        }
        update_result(p);
    }

    p->last_beat_n    = beat_n;
    p->have_last_beat = true;
    reset_extremes(p);
}

/* Implements: REQ-PRC-01, REQ-PRC-02, REQ-PRC-04, REQ-FLT-02 (detection) */
bool ppg_process(ppg_t *p, uint32_t red, uint32_t ir)
{
    const float x_ir  = (float)ir;
    const float x_red = (float)red;
    const float fs    = (float)p->fs;
    bool beat = false;

    p->n++;

    /* 1. DC tracking, re-acquiring immediately after a large level change. */
    if (!p->dc_init || (absf(x_ir - p->ir_dc) > (DC_JUMP_FRACTION * p->ir_dc))) {
        p->ir_dc  = x_ir;   p->red_dc = x_red;
        p->ir_lp  = x_ir;   p->red_lp = x_red;
        p->dc_init = true;
        reset_beats(p);
        reset_extremes(p);
    } else {
        const float a = 1.0f / (DC_TIME_CONSTANT_S * fs);
        p->ir_dc  += a * (x_ir  - p->ir_dc);
        p->red_dc += a * (x_red - p->red_dc);
        p->ir_lp  += LP_ALPHA * (x_ir  - p->ir_lp);
        p->red_lp += LP_ALPHA * (x_red - p->red_lp);
    }

    /* 2. Finger detection: with no finger the sensor sees only ambient light. */
    p->result.finger_present = (p->ir_dc >= FINGER_MIN_DC);
    if (!p->result.finger_present) {
        reset_beats(p);
        return false;
    }

    /* 3. Track this beat's extremes for the AC amplitude. */
    if (p->ir_lp  > p->ir_max)  { p->ir_max  = p->ir_lp;  }
    if (p->ir_lp  < p->ir_min)  { p->ir_min  = p->ir_lp;  }
    if (p->red_lp > p->red_max) { p->red_max = p->red_lp; }
    if (p->red_lp < p->red_min) { p->red_min = p->red_lp; }

    /* 4. Beat detection. The signal dips at each beat, so invert it. */
    const float ac = p->ir_dc - p->ir_lp;
    if (ac > p->env) {
        p->env = ac;
    } else {
        p->env -= p->env / (ENV_DECAY_S * fs);
    }
    const float threshold = BEAT_THRESHOLD * p->env;

    if (!p->in_beat) {
        if ((ac > threshold) && (threshold > 0.0f)) {
            p->in_beat     = true;
            p->beat_peak   = ac;
            p->beat_peak_n = p->n;
        }
    } else {
        if (ac > p->beat_peak) {
            p->beat_peak   = ac;
            p->beat_peak_n = p->n;
        }
        if (ac < threshold) {
            p->in_beat = false;
            const uint32_t min_gap = (uint32_t)(MIN_BEAT_INTERVAL_S * fs);
            if (!p->have_last_beat || ((p->beat_peak_n - p->last_beat_n) >= min_gap)) {
                on_beat(p, p->beat_peak_n);
                beat = true;
            }
        }
    }

    /* 5. Signal lost: no beat for too long. */
    if (p->have_last_beat &&
        ((p->n - p->last_beat_n) > (uint32_t)(MAX_BEAT_INTERVAL_S * fs))) {
        reset_beats(p);
    }

    return beat;
}
