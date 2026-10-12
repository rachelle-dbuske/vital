#ifndef SENSOR_H
#define SENSOR_H

#include <stdint.h>

/* One raw sample from the optical sensor. */
typedef struct {
    uint32_t red;    /* red LED channel, raw ADC counts */
    uint32_t ir;     /* infrared LED channel, raw ADC counts */
    uint32_t tick;   /* FreeRTOS tick when sampled (filled by caller) */
} sensor_sample_t;

typedef enum {
    SENSOR_OK = 0,
    SENSOR_ERR_NOT_FOUND,   /* identity check failed (REQ-SNS-02) */
    SENSOR_ERR_COMM         /* bus or read error */
} sensor_status_t;

/* Implemented by either sensor_sim.c or max30102.c. */
sensor_status_t sensor_init(void);
sensor_status_t sensor_read(sensor_sample_t *out);

#endif /* SENSOR_H */
