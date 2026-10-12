#include "watchdog.h"
#include "fault.h"
#include "app_tasks.h"
#include "iwdg.h"

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

#include <stdint.h>
#include <string.h>

#define WD_WINDOW_MS            250U   /* worst-case detection = 2 windows = 500 ms */
#define WD_ALL_BITS             ((EventBits_t)((1UL << WD_COUNT) - 1UL))
#define SUPERVISOR_STACK_WORDS  256U
#define SUPERVISOR_PRIORITY     (tskIDLE_PRIORITY + 5)   /* highest application task */

static StaticEventGroup_t checkinGroupBuf;
static EventGroupHandle_t checkinGroup;

static StaticTask_t supTcb;
static StackType_t  supStack[SUPERVISOR_STACK_WORDS];

static const char *const taskNames[WD_COUNT] = { "sample", "process", "cli" };

#if VITAL_FAULT_INJECTION
static volatile uint32_t injectHangMask = 0U;   /* bit WD_COUNT = supervisor */

bool wd_inject_hang(const char *name)
{
    for (uint32_t i = 0U; i < (uint32_t)WD_COUNT; i++) {
        if (strcmp(name, taskNames[i]) == 0) {
            injectHangMask |= (1UL << i);
            return true;
        }
    }
    if (strcmp(name, "supervisor") == 0) {
        injectHangMask |= (1UL << (uint32_t)WD_COUNT);
        return true;
    }
    return false;
}
#endif

void wd_checkin(wd_task_id_t id)
{
#if VITAL_FAULT_INJECTION
    if ((injectHangMask & (1UL << (uint32_t)id)) != 0U) {
        for (;;) { }   /* simulated infinite loop in the calling task */
    }
#endif
    (void)xEventGroupSetBits(checkinGroup, (EventBits_t)(1UL << (uint32_t)id));
}

/* Implements: REQ-FLT-01, REQ-FLT-05 */
static void supervisor_task(void *arg)
{
    (void)arg;
    fault_report_last();

    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(WD_WINDOW_MS));

#if VITAL_FAULT_INJECTION
        if ((injectHangMask & (1UL << (uint32_t)WD_COUNT)) != 0U) {
            for (;;) { }   /* supervisor hangs: only the IWDG can recover */
        }
#endif

        /* Read and clear in one step, so no check-in is lost in between. */
        const EventBits_t seen    = xEventGroupClearBits(checkinGroup, WD_ALL_BITS);
        const EventBits_t missing = WD_ALL_BITS & ~seen;

        if (missing != 0U) {
            for (uint32_t i = 0U; i < (uint32_t)WD_COUNT; i++) {
                if ((missing & (1UL << i)) != 0U) {
                    safe_state_enter(FAULT_TASK_MISSED_CHECKIN, i, taskNames[i]);
                }
            }
        }

        (void)HAL_IWDG_Refresh(&hiwdg);
    }
}

void wd_create(void)
{
    checkinGroup = xEventGroupCreateStatic(&checkinGroupBuf);
    configASSERT(checkinGroup != NULL);

    const TaskHandle_t h = xTaskCreateStatic(supervisor_task, "supervisor",
                                             SUPERVISOR_STACK_WORDS, NULL,
                                             SUPERVISOR_PRIORITY, supStack, &supTcb);
    configASSERT(h != NULL);
}