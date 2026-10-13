#ifndef PPG_MODEL_H
#define PPG_MODEL_H

/*
 * Synthetic PPG (pulse waveform) generator.
 * Pure C: no FreeRTOS or HAL, so it is shared by the on-target simulator
 * (sensor_sim.c) and the PC unit tests.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Settings: change these at any time */
    float    hr_bpm;        /* heart rate */
    float    spo2;          /* target SpO2, percent */
    float    perfusion;     /* IR AC/DC ratio, e.g. 0.01 = 1% */
    float    noise_counts;  /* noise amplitude, +/- counts */
    bool     finger;        /* finger on the sensor */

    /* Internal state */
    float    fs;            /* sample rate, Hz */
    float    phase;         /* position in the current beat, 0..1 */
    uint32_t rng;           /* pseudo-random generator state */
} ppg_model_t;

/* Defaults: 72 BPM, 98% SpO2, 1% perfusion, 20 counts noise, finger present. */
void ppg_model_init(ppg_model_t *m, float fs, uint32_t seed);

/* Produce the next red and IR sample. */
void ppg_model_next(ppg_model_t *m, uint32_t *red, uint32_t *ir);

#endif /* PPG_MODEL_H */
