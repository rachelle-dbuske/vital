#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

typedef enum {
    FAULT_NONE = 0,
    FAULT_TASK_MISSED_CHECKIN,
    FAULT_STACK_OVERFLOW,
    FAULT_MALLOC_FAILED
} fault_cause_t;

/* Call early in main(), before the scheduler starts. */
void fault_boot_check(void);

/* Call once from a task, when logging works, to report the previous fault. */
void fault_report_last(void);

/* Record the fault, activate the alarm output, stop everything.
 * Never returns: the IWDG resets the chip. */
void safe_state_enter(fault_cause_t cause, uint32_t detail, const char *info);

#endif /* FAULT_H */