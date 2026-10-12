#include "fault.h"
#include "app_tasks.h"
#include "main.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <string.h>

#define FAULT_MAGIC  0xFA017C0DUL

typedef struct {
    uint32_t magic;
    uint32_t cause;
    uint32_t detail;
    uint32_t tick;
    uint32_t check;   /* detects a garbage record after power-on */
} fault_record_t;

/* Lives in .noinit: survives a reset, not touched by startup code. */
static fault_record_t faultRecord __attribute__((section(".noinit")));

static fault_record_t lastRecord;
static bool           haveLastRecord = false;
static bool           lastResetWasIwdg = false;

static uint32_t record_check(const fault_record_t *r)
{
    return ~(r->magic ^ r->cause ^ r->detail ^ r->tick);
}

static const char *cause_name(uint32_t cause)
{
    switch (cause) {
        case FAULT_TASK_MISSED_CHECKIN: return "task missed watchdog check-in";
        case FAULT_STACK_OVERFLOW:      return "stack overflow";
        case FAULT_MALLOC_FAILED:       return "malloc failed";
        default:                        return "unknown";
    }
}

/* Implements: REQ-FLT-04 */
void fault_boot_check(void)
{
    lastResetWasIwdg = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0U);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    if ((faultRecord.magic == FAULT_MAGIC) &&
        (faultRecord.check == record_check(&faultRecord))) {
        lastRecord     = faultRecord;
        haveLastRecord = true;
    }
    (void)memset(&faultRecord, 0, sizeof(faultRecord));
}

void fault_report_last(void)
{
    if (haveLastRecord) {
        app_log("LAST FAULT: %s (detail %lu) at %lu ms%s\r\n",
                cause_name(lastRecord.cause),
                (unsigned long)lastRecord.detail,
                (unsigned long)(lastRecord.tick * portTICK_PERIOD_MS),
                lastResetWasIwdg ? ", followed by watchdog reset" : "");
    } else if (lastResetWasIwdg) {
        app_log("LAST RESET: watchdog, no fault record (supervisor hung?)\r\n");
    } else {
        app_log("Normal startup\r\n");
    }
}

/* Fault path: no mutex, no RTOS calls that could block. */
static void raw_print(const char *s)
{
    (void)HAL_UART_Transmit(&huart1, (const uint8_t *)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

/* Implements: REQ-FLT-01, REQ-FLT-03 */
void safe_state_enter(fault_cause_t cause, uint32_t detail, const char *info)
{
    taskDISABLE_INTERRUPTS();

    faultRecord.magic  = FAULT_MAGIC;
    faultRecord.cause  = (uint32_t)cause;
    faultRecord.detail = detail;
    faultRecord.tick   = (uint32_t)xTaskGetTickCount();
    faultRecord.check  = record_check(&faultRecord);

    /* Alarm output: solid LED stands in for the buzzer/alarm for now. */
    HAL_GPIO_WritePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin, GPIO_PIN_SET);

    raw_print("\r\n*** SAFE STATE: ");
    raw_print(cause_name((uint32_t)cause));
    if (info != NULL) {
        raw_print(" [");
        raw_print(info);
        raw_print("]");
    }
    raw_print(" ***\r\n");

    for (;;) {
        /* Stop kicking the IWDG: it resets the chip within ~1 s. */
    }
}