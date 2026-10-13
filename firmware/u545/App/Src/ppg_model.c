#include "ppg_model.h"

#include <math.h>

#define MODEL_IR_DC           100000.0f
#define MODEL_RED_DC           80000.0f
#define MODEL_NO_FINGER_LEVEL    500.0f

/* Linear congruential generator: returns a value in [-1, 1). */
static float noise(ppg_model_t *m)
{
    m->rng = (m->rng * 1664525u) + 1013904223u;
    return ((float)(m->rng >> 8) / 8388608.0f) - 1.0f;
}

/* One heartbeat: systolic peak plus a smaller dicrotic wave. */
static float pulse_shape(float p)
{
    const float a = (p - 0.20f) / 0.07f;
    const float b = (p - 0.45f) / 0.10f;
    return expf(-(a * a)) + (0.4f * expf(-(b * b)));
}

void ppg_model_init(ppg_model_t *m, float fs, uint32_t seed)
{
    m->hr_bpm       = 72.0f;
    m->spo2         = 98.0f;
    m->perfusion    = 0.01f;
    m->noise_counts = 20.0f;
    m->finger       = true;
    m->fs           = fs;
    m->phase        = 0.0f;
    m->rng          = seed;
}

void ppg_model_next(ppg_model_t *m, uint32_t *red, uint32_t *ir)
{
    if (!m->finger) {
        *ir  = (uint32_t)(MODEL_NO_FINGER_LEVEL + (m->noise_counts * noise(m)));
        *red = (uint32_t)(MODEL_NO_FINGER_LEVEL + (m->noise_counts * noise(m)));
        return;
    }

    m->phase += (m->hr_bpm / 60.0f) / m->fs;
    if (m->phase >= 1.0f) {
        m->phase -= 1.0f;
    }

    /* Invert SpO2 = 110 - 25R to get the red/IR ratio R for the target SpO2. */
    const float r      = (110.0f - m->spo2) / 25.0f;
    const float ir_ac  = MODEL_IR_DC * m->perfusion;
    const float red_ac = r * MODEL_RED_DC * m->perfusion;
    const float shape  = pulse_shape(m->phase);

    /* More blood at each beat absorbs more light, so the signal dips. */
    *ir  = (uint32_t)(MODEL_IR_DC  - (ir_ac  * shape) + (m->noise_counts * noise(m)));
    *red = (uint32_t)(MODEL_RED_DC - (red_ac * shape) + (m->noise_counts * noise(m)));
}
