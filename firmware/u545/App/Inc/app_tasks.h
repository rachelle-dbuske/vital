#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdbool.h>

#include "ppg.h"

void app_tasks_create(void);
void app_log(const char *fmt, ...);
void app_print_stats(void);
void app_set_periodic_log(bool enabled);
void app_get_vitals(ppg_result_t *out, uint32_t *published_ms);

#endif /* APP_TASKS_H */