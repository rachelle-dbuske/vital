#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>

typedef enum {
    WD_SAMPLE = 0,
    WD_PROCESS,
    WD_CLI,
    WD_COUNT
} wd_task_id_t;

void wd_create(void);
void wd_checkin(wd_task_id_t id);

#if VITAL_FAULT_INJECTION
/* Make the named task (or "supervisor") hang. Returns false if the name is unknown. */
bool wd_inject_hang(const char *name);
#endif

#endif /* WATCHDOG_H */