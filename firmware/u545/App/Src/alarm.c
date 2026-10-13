#include "alarm.h"

/* Hysteresis: a condition clears only once the value is clearly back in range,
 * so a reading hovering at a limit does not make the alarm chatter. */
#define SPO2_CLEAR_MARGIN   1.0f   /* percent */
#define HR_CLEAR_MARGIN     2.0f   /* BPM */

/* Allowed limit settings (REQ-ALM-05). */
#define SPO2_LOW_MIN        50.0f
#define SPO2_LOW_MAX        99.0f
#define HR_LIMIT_MIN        25.0f
#define HR_LIMIT_MAX       250.0f
#define HR_MIN_GAP          10.0f

/* Wrap-safe check: has time 'deadline' been reached? */
static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

void alarm_init(alarm_t *a)
{
    a->limits.spo2_low = 90.0f;
    a->limits.hr_low   = 50.0f;
    a->limits.hr_high  = 120.0f;
    a->conditions       = 0U;
    a->invalid_timing   = false;
    a->invalid_since_ms = 0U;
    a->paused           = false;
    a->pause_until_ms   = 0U;
}

/* Implements: REQ-ALM-05 */
bool alarm_set_limits(alarm_t *a, const alarm_limits_t *l)
{
    const bool ok =
        (l->spo2_low >= SPO2_LOW_MIN) && (l->spo2_low <= SPO2_LOW_MAX) &&
        (l->hr_low  >= HR_LIMIT_MIN)  && (l->hr_high <= HR_LIMIT_MAX) &&
        ((l->hr_high - l->hr_low) >= HR_MIN_GAP);

    if (ok) {
        a->limits = *l;
    }
    return ok;
}

/* Implements: REQ-ALM-04 */
void alarm_button_pressed(alarm_t *a, uint32_t now_ms)
{
    if (a->conditions != 0U) {
        a->paused         = true;
        a->pause_until_ms = now_ms + ALM_PAUSE_MS;
    }
}

/* Set or clear one condition with hysteresis. */
static void set_cond(alarm_t *a, uint32_t flag, bool enter, bool leave)
{
    if (enter) {
        a->conditions |= flag;
    } else if (leave) {
        a->conditions &= ~flag;
    } else {
        /* between the thresholds: keep the current state */
    }
}

/* Implements: REQ-ALM-01, REQ-ALM-02, REQ-ALM-03, REQ-FLT-02 */
void alarm_update(alarm_t *a, const ppg_result_t *v, uint32_t now_ms, alarm_output_t *out)
{
    const alarm_limits_t *l = &a->limits;

    /* ---- Technical conditions ---- */
    if (!v->finger_present) {
        /* No finger: physiological values are meaningless, so the technical
         * alarm takes over immediately. */
        a->conditions     = ALM_NO_FINGER;
        a->invalid_timing = false;
    } else {
        a->conditions &= ~ALM_NO_FINGER;

        if (v->valid) {
            a->invalid_timing = false;
            a->conditions    &= ~ALM_SIGNAL_LOST;

            /* ---- Physiological conditions (only from valid readings) ---- */
            set_cond(a, ALM_SPO2_LOW,
                     v->spo2 < l->spo2_low,
                     v->spo2 >= (l->spo2_low + SPO2_CLEAR_MARGIN));
            set_cond(a, ALM_HR_LOW,
                     v->hr_bpm < l->hr_low,
                     v->hr_bpm >= (l->hr_low + HR_CLEAR_MARGIN));
            set_cond(a, ALM_HR_HIGH,
                     v->hr_bpm > l->hr_high,
                     v->hr_bpm <= (l->hr_high - HR_CLEAR_MARGIN));
        } else {
            /* Finger on, reading not trustworthy. Physiological conditions are
             * HELD: a patient in alarm is not declared fine just because the
             * signal became noisy. After a delay, signal loss is itself an alarm. */
            if (!a->invalid_timing) {
                a->invalid_timing   = true;
                a->invalid_since_ms = now_ms;
            }
            if (time_reached(now_ms, a->invalid_since_ms + ALM_SIGNAL_LOST_DELAY_MS)) {
                a->conditions |= ALM_SIGNAL_LOST;
            }
        }
    }

    /* ---- Audio pause ---- */
    if (a->paused && ((a->conditions == 0U) || time_reached(now_ms, a->pause_until_ms))) {
        a->paused = false;   /* ends on timeout, or when all alarms clear */
    }

    /* ---- Outputs ---- */
    out->conditions    = a->conditions;
    out->physiological = (a->conditions & ALM_PHYS_MASK) != 0U;
    out->technical     = (a->conditions & ALM_TECH_MASK) != 0U;
    out->audio_paused  = a->paused;
    out->audible       = (a->conditions != 0U) && !a->paused;

    /* Visual: physiological (higher priority) blinks fast, technical slow.
     * The visual alarm stays on while audio is paused. */
    if (out->physiological) {
        out->led_on = (now_ms % ALM_PHYS_BLINK_PERIOD_MS) < (ALM_PHYS_BLINK_PERIOD_MS / 2U);
    } else if (out->technical) {
        out->led_on = (now_ms % ALM_TECH_BLINK_PERIOD_MS) < (ALM_TECH_BLINK_PERIOD_MS / 2U);
    } else {
        out->led_on = false;
    }
}
