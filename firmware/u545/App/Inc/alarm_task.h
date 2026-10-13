#ifndef ALARM_TASK_H
#define ALARM_TASK_H

#include <stdbool.h>
#include "alarm.h"

void alarm_task_create(void);

/* Called by the processing task each time it publishes new vitals. */
void alarm_task_notify_vitals(void);

/* Called from the user button interrupt. */
void alarm_task_button_from_isr(void);

/* CLI access (thread-safe). */
bool alarm_task_set_limits(const alarm_limits_t *limits);
void alarm_task_get_limits(alarm_limits_t *limits);
void alarm_task_get_output(alarm_output_t *out);
void alarm_task_request_pause(void);   /* same effect as pressing the button */

#endif /* ALARM_TASK_H */
