#ifndef ALARM_H
#define ALARM_H

/*
 * Alarm decision logic. Pure C: no FreeRTOS or HAL, so it is unit tested on
 * a PC. The alarm task calls alarm_update() with each new result and on a
 * short timer (for blinking and the pause timeout).
 */

#include <stdbool.h>
#include <stdint.h>
#include "ppg.h"

/* Alarm conditions (bit flags) */
#define ALM_SPO2_LOW      ((uint32_t)1U << 0)   /* physiological */
#define ALM_HR_LOW        ((uint32_t)1U << 1)   /* physiological */
#define ALM_HR_HIGH       ((uint32_t)1U << 2)   /* physiological */
#define ALM_NO_FINGER     ((uint32_t)1U << 3)   /* technical */
#define ALM_SIGNAL_LOST   ((uint32_t)1U << 4)   /* technical */

#define ALM_PHYS_MASK     (ALM_SPO2_LOW | ALM_HR_LOW | ALM_HR_HIGH)
#define ALM_TECH_MASK     (ALM_NO_FINGER | ALM_SIGNAL_LOST)

/* Timing */
#define ALM_PAUSE_MS             120000UL  /* REQ-ALM-04: 2 minutes */
#define ALM_SIGNAL_LOST_DELAY_MS  10000UL  /* finger on but no valid reading */
#define ALM_PHYS_BLINK_PERIOD_MS    250UL  /* fast blink: physiological */
#define ALM_TECH_BLINK_PERIOD_MS   2000UL  /* slow blink: technical */

typedef struct {
    float spo2_low;   /* percent */
    float hr_low;     /* BPM */
    float hr_high;    /* BPM */
} alarm_limits_t;

typedef struct {
    uint32_t conditions;     /* ALM_* flags currently active */
    bool     physiological;  /* any physiological condition */
    bool     technical;      /* any technical condition */
    bool     audio_paused;
    bool     audible;        /* buzzer should sound now */
    bool     led_on;         /* visual indicator state now */
} alarm_output_t;

typedef struct {
    alarm_limits_t limits;
    uint32_t       conditions;
    bool           invalid_timing;
    uint32_t       invalid_since_ms;
    bool           paused;
    uint32_t       pause_until_ms;
} alarm_t;

/* Defaults: SpO2 < 90%, HR outside 50-120 BPM. */
void alarm_init(alarm_t *a);

/* Returns false (and changes nothing) if the limits are unsafe or inconsistent. */
bool alarm_set_limits(alarm_t *a, const alarm_limits_t *limits);

/* User pressed the alarm pause button. Ignored when no alarm is active. */
void alarm_button_pressed(alarm_t *a, uint32_t now_ms);

/* Evaluate the latest vitals and compute the outputs. */
void alarm_update(alarm_t *a, const ppg_result_t *v, uint32_t now_ms, alarm_output_t *out);

#endif /* ALARM_H */
