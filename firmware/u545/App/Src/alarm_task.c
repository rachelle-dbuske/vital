#include "alarm_task.h"
#include "app_tasks.h"
#include "watchdog.h"
#include "main.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>

/* ---------- Configuration ---------- */
#define ALARM_TASK_STACK_WORDS   384U
#define ALARM_TASK_PRIORITY      (tskIDLE_PRIORITY + 5)  /* above sampling, below supervisor */
#define ALARM_TICK_MS            25U    /* LED blink resolution and pause timing */
#define BUTTON_DEBOUNCE_MS       200U
#define STARTUP_TEST_MS          1000U  /* REQ-UI-02 */

#define NOTIFY_VITALS            (1UL << 0)
#define NOTIFY_BUTTON            (1UL << 1)

/* ---------- State ---------- */
static StaticTask_t   alarmTcb;
static StackType_t    alarmStack[ALARM_TASK_STACK_WORDS];
static TaskHandle_t   alarmTaskHandle = NULL;

static alarm_t        alarmState;      /* written only by the alarm task, except limits */
static alarm_output_t alarmOut;        /* latest outputs, read by the CLI */
static volatile bool  pauseRequested = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---------- Notifications ---------- */
void alarm_task_notify_vitals(void)
{
    if (alarmTaskHandle != NULL) {
        (void)xTaskNotify(alarmTaskHandle, NOTIFY_VITALS, eSetBits);
    }
}

void alarm_task_button_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (alarmTaskHandle != NULL) {
        (void)xTaskNotifyFromISR(alarmTaskHandle, NOTIFY_BUTTON, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ---------- CLI access ---------- */
bool alarm_task_set_limits(const alarm_limits_t *limits)
{
    taskENTER_CRITICAL();
    const bool ok = alarm_set_limits(&alarmState, limits);
    taskEXIT_CRITICAL();
    return ok;
}

void alarm_task_get_limits(alarm_limits_t *limits)
{
    taskENTER_CRITICAL();
    *limits = alarmState.limits;
    taskEXIT_CRITICAL();
}

void alarm_task_get_output(alarm_output_t *out)
{
    taskENTER_CRITICAL();
    *out = alarmOut;
    taskEXIT_CRITICAL();
}

void alarm_task_request_pause(void)
{
    pauseRequested = true;
    if (alarmTaskHandle != NULL) {
        (void)xTaskNotify(alarmTaskHandle, NOTIFY_BUTTON, eSetBits);
    }
}

/* ---------- Outputs ---------- */
static void drive_outputs(bool led, bool audio)
{
    HAL_GPIO_WritePin(LED_GREEN_GPIO_Port,   LED_GREEN_Pin,   led   ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ALARM_AUDIO_GPIO_Port, ALARM_AUDIO_Pin, audio ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void log_changes(const alarm_output_t *prev, const alarm_output_t *cur, uint32_t latency_ms)
{
    const uint32_t added   = cur->conditions & ~prev->conditions;
    const uint32_t cleared = prev->conditions & ~cur->conditions;

    if ((added & ALM_SPO2_LOW) != 0U)    { app_log("ALARM: SpO2 low (latency %lu ms)\r\n", (unsigned long)latency_ms); }
    if ((added & ALM_HR_LOW) != 0U)      { app_log("ALARM: HR low (latency %lu ms)\r\n",   (unsigned long)latency_ms); }
    if ((added & ALM_HR_HIGH) != 0U)     { app_log("ALARM: HR high (latency %lu ms)\r\n",  (unsigned long)latency_ms); }
    if ((added & ALM_NO_FINGER) != 0U)   { app_log("TECH ALARM: no finger\r\n"); }
    if ((added & ALM_SIGNAL_LOST) != 0U) { app_log("TECH ALARM: signal lost\r\n"); }
    if ((cleared != 0U) && (cur->conditions == 0U)) { app_log("alarms cleared\r\n"); }
    if (cur->audio_paused && !prev->audio_paused)   { app_log("audio paused for 2 min\r\n"); }
    if (!cur->audio_paused && prev->audio_paused && (cur->conditions != 0U)) {
        app_log("audio pause ended\r\n");
    }
}

/* ---------- Task ---------- */
/* Implements: REQ-ALM-01..05, REQ-UI-02, REQ-FLT-02 (alarm output) */
static void alarm_task(void *arg)
{
    (void)arg;
    uint32_t       lastPressMs = 0U;
    alarm_output_t prev        = { 0 };
    ppg_result_t   vitals;
    uint32_t       publishedMs;

    /* Startup self-test of the alarm outputs (REQ-UI-02).
     * Wait in short steps and keep checking in: a single 1 s delay would
     * look like a hung task to the supervisor (250 ms window). */
    drive_outputs(true, true);
    for (uint32_t t = 0U; t < STARTUP_TEST_MS; t += ALARM_TICK_MS) {
        wd_checkin(WD_ALARM);
        vTaskDelay(pdMS_TO_TICKS(ALARM_TICK_MS));
    }
    drive_outputs(false, false);

    for (;;) {
        wd_checkin(WD_ALARM);

        uint32_t bits = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &bits, pdMS_TO_TICKS(ALARM_TICK_MS));
        const uint32_t now = now_ms();

        /* Button (debounced) or CLI pause request */
        if ((bits & NOTIFY_BUTTON) != 0U) {
            if (pauseRequested || ((now - lastPressMs) >= BUTTON_DEBOUNCE_MS)) {
                lastPressMs = now;
                taskENTER_CRITICAL();
                alarm_button_pressed(&alarmState, now);
                taskEXIT_CRITICAL();
            }
            pauseRequested = false;
        }

        /* Evaluate the latest vitals */
        app_get_vitals(&vitals, &publishedMs);
        alarm_output_t cur;
        taskENTER_CRITICAL();
        alarm_update(&alarmState, &vitals, now, &cur);
        alarmOut = cur;
        taskEXIT_CRITICAL();

        drive_outputs(cur.led_on, cur.audible);

        /* Test point low: this result has been acted on (pulse width = latency) */
        HAL_GPIO_WritePin(TP_VITALS_GPIO_Port, TP_VITALS_Pin, GPIO_PIN_RESET);

        if ((cur.conditions != prev.conditions) || (cur.audio_paused != prev.audio_paused)) {
            log_changes(&prev, &cur, now - publishedMs);
        }
        prev = cur;
    }
}

void alarm_task_create(void)
{
    alarm_init(&alarmState);
    alarmTaskHandle = xTaskCreateStatic(alarm_task, "alarm", ALARM_TASK_STACK_WORDS, NULL,
                                        ALARM_TASK_PRIORITY, alarmStack, &alarmTcb);
    configASSERT(alarmTaskHandle != NULL);
}
