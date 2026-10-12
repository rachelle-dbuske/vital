#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdbool.h>

void app_tasks_create(void);
void app_log(const char *fmt, ...);
void app_print_stats(void);
void app_set_periodic_log(bool enabled);

#endif /* APP_TASKS_H */