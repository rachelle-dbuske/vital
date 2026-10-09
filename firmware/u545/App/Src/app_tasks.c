#include "app_tasks.h"
#include "main.h"
#include "usart.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdarg.h>
#include <stdio.h>

/* ---------- Task configuration ---------- */
#define LED_TASK_STACK_WORDS  128U   /* 512 bytes */
#define HB_TASK_STACK_WORDS   512U   /* 2 KB: printf with floats needs room */

#define LED_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)
#define HB_TASK_PRIORITY      (tskIDLE_PRIORITY + 2)

/* ---------- Static storage (no heap) ---------- */
static StaticTask_t  ledTaskTcb;
static StackType_t   ledTaskStack[LED_TASK_STACK_WORDS];
static TaskHandle_t  ledTaskHandle;

static StaticTask_t  hbTaskTcb;
static StackType_t   hbTaskStack[HB_TASK_STACK_WORDS];
static TaskHandle_t  hbTaskHandle;

static StaticSemaphore_t uartMutexBuf;
static SemaphoreHandle_t uartMutex;

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
        len = (int)sizeof(buf) - 1;   /* message was truncated */
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
    const TickType_t period = pdMS_TO_TICKS(500);

    for (;;) {
        HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin);
        vTaskDelayUntil(&lastWake, period);
    }
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000);
    uint32_t count = 0;

    for (;;) {
        app_log("[%lu ms] heartbeat %lu | stack free (words): led=%lu hb=%lu\r\n",
                (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                (unsigned long)count++,
                (unsigned long)uxTaskGetStackHighWaterMark(ledTaskHandle),
                (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelayUntil(&lastWake, period);
    }
}

/* ---------- Creation (call before the scheduler starts) ---------- */
void app_tasks_create(void)
{
    uartMutex = xSemaphoreCreateMutexStatic(&uartMutexBuf);
    configASSERT(uartMutex != NULL);

    ledTaskHandle = xTaskCreateStatic(led_task, "led",
                                      LED_TASK_STACK_WORDS, NULL,
                                      LED_TASK_PRIORITY,
                                      ledTaskStack, &ledTaskTcb);

    hbTaskHandle = xTaskCreateStatic(heartbeat_task, "heartbeat",
                                     HB_TASK_STACK_WORDS, NULL,
                                     HB_TASK_PRIORITY,
                                     hbTaskStack, &hbTaskTcb);

    configASSERT(ledTaskHandle != NULL);
    configASSERT(hbTaskHandle != NULL);
}