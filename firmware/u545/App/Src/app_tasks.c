#include "app_tasks.h"
#include "main.h"
#include "usart.h"
#include "sensor.h"
#include "watchdog.h"
#include "fault.h"
#include "ppg.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

/* ---------- Configuration ---------- */
#define SAMPLE_PERIOD_MS          10U    /* 100 Hz (REQ-SNS-01) */
#define SAMPLE_QUEUE_LENGTH       32U    /* 320 ms of buffering */
#define STATS_WINDOW_SAMPLES     100U    /* 1 s of samples */
#define STREAM_RAW                 0     /* 1 = stream samples for plotting */

#define LED_TASK_STACK_WORDS     128U
#define HB_TASK_STACK_WORDS      512U
#define SAMPLE_TASK_STACK_WORDS  256U
#define PROC_TASK_STACK_WORDS    512U

#define LED_TASK_PRIORITY        (tskIDLE_PRIORITY + 1)
#define HB_TASK_PRIORITY         (tskIDLE_PRIORITY + 2)
#define PROC_TASK_PRIORITY       (tskIDLE_PRIORITY + 3)
#define SAMPLE_TASK_PRIORITY     (tskIDLE_PRIORITY + 4)

/* ---------- Static storage (no heap) ---------- */
static StaticTask_t  ledTaskTcb;
static StackType_t   ledTaskStack[LED_TASK_STACK_WORDS];
static TaskHandle_t  ledTaskHandle;

static StaticTask_t  hbTaskTcb;
static StackType_t   hbTaskStack[HB_TASK_STACK_WORDS];
static TaskHandle_t  hbTaskHandle;

static StaticTask_t  sampleTaskTcb;
static StackType_t   sampleTaskStack[SAMPLE_TASK_STACK_WORDS];
static TaskHandle_t  sampleTaskHandle;

static StaticTask_t  procTaskTcb;
static StackType_t   procTaskStack[PROC_TASK_STACK_WORDS];
static TaskHandle_t  procTaskHandle;

static StaticSemaphore_t uartMutexBuf;
static SemaphoreHandle_t uartMutex;

static StaticQueue_t sampleQueueBuf;
static uint8_t       sampleQueueStorage[SAMPLE_QUEUE_LENGTH * sizeof(sensor_sample_t)];
static QueueHandle_t sampleQueue;

/* Diagnostics */
static volatile uint32_t droppedSamples = 0;   /* REQ-SNS-03 */
static volatile uint32_t readErrors     = 0;
static volatile bool periodicLog = true;

static ppg_t        ppg;            /* static: too large for the task stack */
static ppg_result_t latestResult;   /* shared with other tasks */

/* ---------- Thread-safe logging ---------- */
void app_log(const char *fmt, ...)
{
    char buf[128];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) {
        return;
    }
    if (len >= (int)sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }

    xSemaphoreTake(uartMutex, portMAX_DELAY);
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, HAL_MAX_DELAY);
    xSemaphoreGive(uartMutex);
}

/* ---------- Tasks ---------- */
static void led_task(void *arg)
{
    (void)arg;
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(500));
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (periodicLog) {
            app_log("[%lu ms] stack free (words): led=%lu hb=%lu smp=%lu proc=%lu\r\n",
                    (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                    (unsigned long)uxTaskGetStackHighWaterMark(ledTaskHandle),
                    (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                    (unsigned long)uxTaskGetStackHighWaterMark(sampleTaskHandle),
                    (unsigned long)uxTaskGetStackHighWaterMark(procTaskHandle));
        }
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(5000));
    }
}

/* Implements: REQ-SNS-01, REQ-SNS-02, REQ-SNS-03 */
static void sampling_task(void *arg)
{
    (void)arg;
    sensor_sample_t sample;

    if (sensor_init() != SENSOR_OK) {
        app_log("ERROR: sensor init failed\r\n");   /* later: technical alarm */
        vTaskSuspend(NULL);
    }

    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (sensor_read(&sample) == SENSOR_OK) {
            sample.tick = (uint32_t)xTaskGetTickCount();

            /* Never block here: if the consumer falls behind, count the loss. */
            if (xQueueSend(sampleQueue, &sample, 0) != pdPASS) {
                droppedSamples++;
            }
        } else {
            readErrors++;
        }
        wd_checkin(WD_SAMPLE);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));

        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

/* Implements: REQ-PRC-03 (1 s update) */
static void processing_task(void *arg)
{
    (void)arg;
    sensor_sample_t s;
    uint32_t sinceReport = 0U;

    ppg_init(&ppg, 1000U / SAMPLE_PERIOD_MS);

    for (;;) {
        wd_checkin(WD_PROCESS);
        if (xQueueReceive(sampleQueue, &s, pdMS_TO_TICKS(100)) != pdPASS) {
            continue;
        }

#if STREAM_RAW
        if (periodicLog) {
            app_log(">ir:%lu\n>red:%lu\n", (unsigned long)s.ir, (unsigned long)s.red);
        }
#endif

        (void)ppg_process(&ppg, s.red, s.ir);

        sinceReport++;
        if (sinceReport >= STATS_WINDOW_SAMPLES) {   /* once per second */
            sinceReport = 0U;
            const ppg_result_t *r = ppg_result(&ppg);

            taskENTER_CRITICAL();
            latestResult = *r;
            taskEXIT_CRITICAL();

#if !STREAM_RAW
            if (periodicLog) {
                if (!r->finger_present) {
                    app_log("no finger | drop=%lu\r\n", (unsigned long)droppedSamples);
                } else if (!r->valid) {
                    app_log("acquiring... perf=%.2f%% | drop=%lu\r\n",
                            r->perfusion * 100.0f, (unsigned long)droppedSamples);
                } else {
                    app_log("HR %.0f BPM | SpO2 %.1f%% | perf %.2f%% | drop=%lu\r\n",
                            r->hr_bpm, r->spo2, r->perfusion * 100.0f,
                            (unsigned long)droppedSamples);
                }
            }
#endif
        }
    }
}

/* ---------- Creation (call before the scheduler starts) ---------- */
void app_tasks_create(void)
{
    uartMutex = xSemaphoreCreateMutexStatic(&uartMutexBuf);
    configASSERT(uartMutex != NULL);

    sampleQueue = xQueueCreateStatic(SAMPLE_QUEUE_LENGTH, sizeof(sensor_sample_t),
                                     sampleQueueStorage, &sampleQueueBuf);
    configASSERT(sampleQueue != NULL);

    ledTaskHandle = xTaskCreateStatic(led_task, "led", LED_TASK_STACK_WORDS, NULL,
                                      LED_TASK_PRIORITY, ledTaskStack, &ledTaskTcb);

    hbTaskHandle = xTaskCreateStatic(heartbeat_task, "heartbeat", HB_TASK_STACK_WORDS, NULL,
                                     HB_TASK_PRIORITY, hbTaskStack, &hbTaskTcb);

    sampleTaskHandle = xTaskCreateStatic(sampling_task, "sample", SAMPLE_TASK_STACK_WORDS, NULL,
                                         SAMPLE_TASK_PRIORITY, sampleTaskStack, &sampleTaskTcb);

    procTaskHandle = xTaskCreateStatic(processing_task, "process", PROC_TASK_STACK_WORDS, NULL,
                                       PROC_TASK_PRIORITY, procTaskStack, &procTaskTcb);

    configASSERT(ledTaskHandle    != NULL);
    configASSERT(hbTaskHandle     != NULL);
    configASSERT(sampleTaskHandle != NULL);
    configASSERT(procTaskHandle   != NULL);
}

void app_set_periodic_log(bool enabled)
{
    periodicLog = enabled;
}

void app_print_stats(void)
{
    app_log("uptime: %lu ms\r\n",
            (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS));
    app_log("samples dropped: %lu | read errors: %lu\r\n",
            (unsigned long)droppedSamples, (unsigned long)readErrors);
    app_log("stack free (words): led=%lu hb=%lu smp=%lu proc=%lu\r\n",
            (unsigned long)uxTaskGetStackHighWaterMark(ledTaskHandle),
            (unsigned long)uxTaskGetStackHighWaterMark(hbTaskHandle),
            (unsigned long)uxTaskGetStackHighWaterMark(sampleTaskHandle),
            (unsigned long)uxTaskGetStackHighWaterMark(procTaskHandle));
}

void app_get_vitals(ppg_result_t *out)
{
    taskENTER_CRITICAL();
    *out = latestResult;
    taskEXIT_CRITICAL();
}
