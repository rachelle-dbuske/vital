#include "sensor.h"
#include "sensor_sim.h"
#include "ppg_model.h"

#include "FreeRTOS.h"
#include "task.h"

#define SIM_SAMPLE_RATE_HZ  100.0f
#define SIM_SEED            12345u

static ppg_model_t model;

sensor_status_t sensor_init(void)
{
    ppg_model_init(&model, SIM_SAMPLE_RATE_HZ, SIM_SEED);
    return SENSOR_OK;
}

sensor_status_t sensor_read(sensor_sample_t *out)
{
    /* The CLI task may change settings at any time: generate atomically. */
    taskENTER_CRITICAL();
    ppg_model_next(&model, &out->red, &out->ir);
    taskEXIT_CRITICAL();
    return SENSOR_OK;
}

void sensor_sim_set_hr(float bpm)
{
    taskENTER_CRITICAL();
    model.hr_bpm = bpm;
    taskEXIT_CRITICAL();
}

void sensor_sim_set_spo2(float percent)
{
    taskENTER_CRITICAL();
    model.spo2 = percent;
    taskEXIT_CRITICAL();
}

void sensor_sim_set_finger_present(bool present)
{
    taskENTER_CRITICAL();
    model.finger = present;
    taskEXIT_CRITICAL();
}

void sensor_sim_set_perfusion(float fraction)
{
    taskENTER_CRITICAL();
    model.perfusion = fraction;
    taskEXIT_CRITICAL();
}

void sensor_sim_set_noise(float counts)
{
    taskENTER_CRITICAL();
    model.noise_counts = counts;
    taskEXIT_CRITICAL();
}
