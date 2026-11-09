#include "app_tasks.h"
#include "main.h"
#include "usart.h"
#include "sensor.h"
#include "watchdog.h"
#include "fault.h"
#include "ppg.h"
#include "alarm_task.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

/* ---------- Configuration ---------- */
#define SAMPLE_PERIOD_MS          10U    /* 100 Hz (REQ-SNS-01) */
#define SAMPLE_NOTIFY_TIMEOUT_MS 100U
#define SAMPLE_QUEUE_LENGTH       32U    /* 320 ms of buffering */
#define LOG_QUEUE_LENGTH          32U
#define LOG_MESSAGE_SIZE         128U
#define STATS_WINDOW_SAMPLES     100U    /* 1 s of samples */
#define STREAM_RAW                 0     /* 1 = stream samples for plotting */

#define HB_TASK_STACK_WORDS      512U
#define SAMPLE_TASK_STACK_WORDS  256U
#define PROC_TASK_STACK_WORDS    512U
#define UART_TASK_STACK_WORDS    384U

#define HB_TASK_PRIORITY         (tskIDLE_PRIORITY + 2)
#define PROC_TASK_PRIORITY       (tskIDLE_PRIORITY + 3)
#define SAMPLE_TASK_PRIORITY     (tskIDLE_PRIORITY + 4)
#define UART_TASK_PRIORITY       (tskIDLE_PRIORITY + 1)

typedef struct {
    uint16_t length;
    char text[LOG_MESSAGE_SIZE];
} app_log_message_t;

/* ---------- Static storage (no heap) ---------- */
static StaticTask_t  hbTaskTcb;
static StackType_t   hbTaskStack[HB_TASK_STACK_WORDS];
static TaskHandle_t  hbTaskHandle;

static StaticTask_t  sampleTaskTcb;
static StackType_t   sampleTaskStack[SAMPLE_TASK_STACK_WORDS];
static TaskHandle_t  sampleTaskHandle;

static StaticTask_t  procTaskTcb;
static StackType_t   procTaskStack[PROC_TASK_STACK_WORDS];
static TaskHandle_t  procTaskHandle;

static StaticTask_t  uartTaskTcb;
static StackType_t   uartTaskStack[UART_TASK_STACK_WORDS];
static TaskHandle_t  uartTaskHandle;

static StaticQueue_t sampleQueueBuf;
static uint8_t       sampleQueueStorage[SAMPLE_QUEUE_LENGTH * sizeof(sensor_sample_t)];
static QueueHandle_t sampleQueue;

static StaticQueue_t logQueueBuf;
static uint8_t       logQueueStorage[LOG_QUEUE_LENGTH * sizeof(app_log_message_t)];
static QueueHandle_t logQueue;

/* Diagnostics */
static volatile uint32_t droppedSamples = 0;   /* REQ-SNS-03 */
static volatile uint32_t readErrors     = 0;
static volatile uint32_t droppedLogMessages = 0;
static volatile bool periodicLog = true;

static ppg_t        ppg;            /* static: too large for the task stack */
static ppg_result_t latestResult;   /* shared with other tasks */

static uint32_t latestPublishedMs;

static void queue_sample(sensor_sample_t *sample)
{
    sample->tick = (uint32_t)xTaskGetTickCount();
    if (xQueueSend(sampleQueue, sample, 0) != pdPASS) {
        droppedSamples++;
    }
}

/* ---------- Thread-safe logging ---------- */
void app_log(const char *fmt, ...)
{
    if (logQueue == NULL) {
        return;
    }

    app_log_message_t message;
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(message.text, sizeof(message.text), fmt, args);
    va_end(args);

    if (len <= 0) {
        return;
    }
    if (len >= (int)sizeof(message.text)) {
        len = (int)sizeof(message.text) - 1;
    }

    message.length = (uint16_t)len;
    if (xQueueSend(logQueue, &message, 0U) != pdPASS) {
        taskENTER_CRITICAL();
        droppedLogMessages++;
        taskEXIT_CRITICAL();
    }
}

static void uart_output_task(void *arg)
{
    (void)arg;
    app_log_message_t message;

    for (;;) {
        if (xQueueReceive(logQueue, &message, portMAX_DELAY) == pdPASS) {
            (void)HAL_UART_Transmit(&huart1, (uint8_t *)message.text,
                                    message.length, HAL_MAX_DELAY);
        }
    }
}

/* ---------- Tasks ---------- */
static void heartbeat_task(void *arg)
{
    (void)arg;
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (periodicLog) {
            app_log("[%lu ms] stack free (words): hb=%lu smp=%lu proc=%lu\r\n",
                    (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
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

#if VITAL_USE_SIM_SENSOR
    TickType_t lastWake = xTaskGetTickCount();
#endif

    for (;;) {
#if VITAL_USE_SIM_SENSOR
        if (sensor_read(&sample) == SENSOR_OK) {
            queue_sample(&sample);
        } else {
            readErrors++;
        }
        wd_checkin(WD_SAMPLE);
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
#else
        const uint32_t notifications = (uint32_t)ulTaskNotifyTake(
            pdTRUE, pdMS_TO_TICKS(SAMPLE_NOTIFY_TIMEOUT_MS));
        wd_checkin(WD_SAMPLE);
        if (notifications == 0U) {
            continue;
        }

        for (;;) {
            const sensor_status_t status = sensor_read(&sample);
            if (status == SENSOR_NO_DATA) {
                break;
            }
            if (status != SENSOR_OK) {
                readErrors++;
                break;
            }

            queue_sample(&sample);
            wd_checkin(WD_SAMPLE);
        }
#endif
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
        const ppg_result_t *r = ppg_result(&ppg);

        sinceReport++;
        const bool periodic = (sinceReport >= STATS_WINDOW_SAMPLES);
        const bool changed  = (r->finger_present != latestResult.finger_present) ||
                              (r->valid != latestResult.valid);

        /* Publish once per second (REQ-PRC-03), and immediately when the
         * finger or validity state changes (REQ-FLT-02: 1 s detection). */
        if (periodic || changed) {
            HAL_GPIO_WritePin(TP_VITALS_GPIO_Port, TP_VITALS_Pin, GPIO_PIN_SET);

            taskENTER_CRITICAL();
            latestResult      = *r;
            latestPublishedMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            taskEXIT_CRITICAL();

            alarm_task_notify_vitals();
        }

        if (periodic) {
            sinceReport = 0U;

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
    logQueue = xQueueCreateStatic(LOG_QUEUE_LENGTH, sizeof(app_log_message_t),
                                  logQueueStorage, &logQueueBuf);
    configASSERT(logQueue != NULL);

    sampleQueue = xQueueCreateStatic(SAMPLE_QUEUE_LENGTH, sizeof(sensor_sample_t),
                                     sampleQueueStorage, &sampleQueueBuf);
    configASSERT(sampleQueue != NULL);

    uartTaskHandle = xTaskCreateStatic(uart_output_task, "uart_tx", UART_TASK_STACK_WORDS, NULL,
                                       UART_TASK_PRIORITY, uartTaskStack, &uartTaskTcb);

    hbTaskHandle = xTaskCreateStatic(heartbeat_task, "heartbeat", HB_TASK_STACK_WORDS, NULL,
                                     HB_TASK_PRIORITY, hbTaskStack, &hbTaskTcb);

    sampleTaskHandle = xTaskCreateStatic(sampling_task, "sample", SAMPLE_TASK_STACK_WORDS, NULL,
                                         SAMPLE_TASK_PRIORITY, sampleTaskStack, &sampleTaskTcb);

    procTaskHandle = xTaskCreateStatic(processing_task, "process", PROC_TASK_STACK_WORDS, NULL,
                                       PROC_TASK_PRIORITY, procTaskStack, &procTaskTcb);

    configASSERT(uartTaskHandle   != NULL);
    configASSERT(hbTaskHandle     != NULL);
    configASSERT(sampleTaskHandle != NULL);
    configASSERT(procTaskHandle   != NULL);
}

void app_sample_task_notify_from_isr(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (sampleTaskHandle != NULL) {
        (void)vTaskNotifyGiveFromISR(sampleTaskHandle, &higherPriorityTaskWoken);
    }
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
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
        app_log("log queue: %lu/%u | dropped log messages: %lu\r\n",
            (unsigned long)uxQueueMessagesWaiting(logQueue), (unsigned int)LOG_QUEUE_LENGTH,
            (unsigned long)droppedLogMessages);
    app_log("stack free (words): hb=%lu smp=%lu proc=%lu\r\n",
            (unsigned long)uxTaskGetStackHighWaterMark(hbTaskHandle),
            (unsigned long)uxTaskGetStackHighWaterMark(sampleTaskHandle),
            (unsigned long)uxTaskGetStackHighWaterMark(procTaskHandle));
}

void app_get_vitals(ppg_result_t *out, uint32_t *published_ms)
{
    taskENTER_CRITICAL();
    *out          = latestResult;
    *published_ms = latestPublishedMs;
    taskEXIT_CRITICAL();
}
