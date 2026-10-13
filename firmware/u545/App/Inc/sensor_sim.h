#ifndef SENSOR_SIM_H
#define SENSOR_SIM_H

#include <stdbool.h>

/* Change the simulated signal at runtime (e.g., from a CLI or test). */
void sensor_sim_set_hr(float bpm);
void sensor_sim_set_spo2(float percent);
void sensor_sim_set_finger_present(bool present);
void sensor_sim_set_perfusion(float fraction);   /* 0.01 = 1%, 0 = no pulse */
void sensor_sim_set_noise(float counts);

#endif /* SENSOR_SIM_H */