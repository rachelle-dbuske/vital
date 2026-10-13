#ifndef PPG_H
#define PPG_H

/*
 * Heart rate and SpO2 estimation from raw red/IR PPG samples.
 * Pure C: no FreeRTOS or HAL, so it runs unchanged on the target and in
 * PC unit tests. Call ppg_process() once per sample, at a fixed rate.
 */

#include <stdbool.h>
#include <stdint.h>

#define PPG_HISTORY  4U   /* beats averaged for HR and SpO2 */

typedef struct {
    bool     finger_present;  /* false: sensor reads ambient light only */
    bool     valid;           /* true: hr_bpm and spo2 can be trusted */
    float    hr_bpm;
    float    spo2;            /* percent */
    float    perfusion;       /* IR AC/DC of the last beat (signal strength) */
    uint32_t beat_count;      /* increments on every detected beat */
} ppg_result_t;

typedef struct {
    uint32_t fs;              /* sample rate, Hz */
    uint32_t n;               /* samples processed */

    /* Filters */
    bool  dc_init;
    float ir_dc,  red_dc;     /* slow average: the steady (DC) level */
    float ir_lp,  red_lp;     /* fast smoothing: removes sample noise */

    /* Beat detection on the IR signal */
    float    env;             /* decaying envelope of beat height */
    bool     in_beat;
    float    beat_peak;
    uint32_t beat_peak_n;
    uint32_t last_beat_n;
    bool     have_last_beat;

    /* Per-beat extremes of the smoothed signals */
    float ir_max, ir_min, red_max, red_min;

    /* History of the last few beats */
    uint32_t intervals[PPG_HISTORY];
    float    ratios[PPG_HISTORY];
    uint32_t hist_count;
    uint32_t hist_idx;

    ppg_result_t result;
} ppg_t;

void ppg_init(ppg_t *p, uint32_t fs);

/* Process one sample. Returns true when a new beat was detected. */
bool ppg_process(ppg_t *p, uint32_t red, uint32_t ir);

/* Latest result (always safe to call). */
const ppg_result_t *ppg_result(const ppg_t *p);

#endif /* PPG_H */
