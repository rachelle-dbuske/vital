#include "sensor.h"
#include "sensor_sim.h"

#include "FreeRTOS.h"
#include "task.h"

#include <math.h>
#include <stdbool.h>

/* ---------- Simulation constants ---------- */
#define SIM_SAMPLE_RATE_HZ    100.0f
#define SIM_IR_DC          100000.0f   /* typical raw level, within the 18-bit ADC range */
#define SIM_RED_DC          80000.0f
#define SIM_PERFUSION           0.01f  /* IR AC/DC ratio (1% perfusion index) */
#define SIM_NOISE_COUNTS       20.0f
#define SIM_NO_FINGER_LEVEL   500.0f   /* ambient level with no finger present */

typedef struct {
    float hr_bpm;
    float spo2;
    bool  finger;
} sim_params_t;

static sim_params_t params = { 72.0f, 98.0f, true };
static float    phase     = 0.0f;      /* position within one heartbeat, 0..1 */
static uint32_t rng_state = 12345u;

/* Small pseudo-random generator: returns a value in [-1, 1). */
static float noise(void)
{
    rng_state = (rng_state * 1664525u) + 1013904223u;
    return ((float)(rng_state >> 8) / 8388608.0f) - 1.0f;
}

/* One heartbeat: main systolic peak plus a smaller dicrotic wave. */
static float pulse_shape(float p)
{
    const float a = (p - 0.20f) / 0.07f;
    const float b = (p - 0.45f) / 0.10f;
    return expf(-(a * a)) + (0.4f * expf(-(b * b)));
}

sensor_status_t sensor_init(void)
{
    phase = 0.0f;
    return SENSOR_OK;
}

sensor_status_t sensor_read(sensor_sample_t *out)
{
    sim_params_t p;

    taskENTER_CRITICAL();
    p = params;                         /* consistent snapshot */
    taskEXIT_CRITICAL();

    if (!p.finger) {
        out->ir  = (uint32_t)(SIM_NO_FINGER_LEVEL + (SIM_NOISE_COUNTS * noise()));
        out->red = (uint32_t)(SIM_NO_FINGER_LEVEL + (SIM_NOISE_COUNTS * noise()));
        return SENSOR_OK;
    }

    phase += (p.hr_bpm / 60.0f) / SIM_SAMPLE_RATE_HZ;
    if (phase >= 1.0f) {
        phase -= 1.0f;
    }

    /* Invert SpO2 = 110 - 25R to get the red/IR ratio R for the target SpO2. */
    const float r      = (110.0f - p.spo2) / 25.0f;
    const float ir_ac  = SIM_IR_DC * SIM_PERFUSION;
    const float red_ac = r * SIM_RED_DC * SIM_PERFUSION;
    const float shape  = pulse_shape(phase);

    /* Blood volume rises at each beat, absorbing more light, so the signal dips. */
    out->ir  = (uint32_t)(SIM_IR_DC  - (ir_ac  * shape) + (SIM_NOISE_COUNTS * noise()));
    out->red = (uint32_t)(SIM_RED_DC - (red_ac * shape) + (SIM_NOISE_COUNTS * noise()));
    return SENSOR_OK;
}

void sensor_sim_set_hr(float bpm)
{
    taskENTER_CRITICAL();
    params.hr_bpm = bpm;
    taskEXIT_CRITICAL();
}

void sensor_sim_set_spo2(float percent)
{
    taskENTER_CRITICAL();
    params.spo2 = percent;
    taskEXIT_CRITICAL();
}

void sensor_sim_set_finger_present(bool present)
{
    taskENTER_CRITICAL();
    params.finger = present;
    taskEXIT_CRITICAL();
}