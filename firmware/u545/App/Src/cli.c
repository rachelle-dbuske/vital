#include "cli.h"
#include "app_tasks.h"
#include "main.h"
#include "usart.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"

#if VITAL_USE_SIM_SENSOR
#include "sensor_sim.h"
#endif
#if VITAL_FAULT_INJECTION
static void cmd_fault(int argc, char *argv[]);
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Configuration ---------- */
#define CLI_TASK_STACK_WORDS   512U
#define CLI_TASK_PRIORITY      (tskIDLE_PRIORITY + 1)
#define CLI_RX_BUFFER_BYTES    64U
#define CLI_LINE_MAX           64U
#define CLI_MAX_ARGS           6

/* ---------- Static storage ---------- */
static StaticTask_t cliTaskTcb;
static StackType_t  cliTaskStack[CLI_TASK_STACK_WORDS];

static StaticStreamBuffer_t rxStreamBuf;
static uint8_t              rxStreamStorage[CLI_RX_BUFFER_BYTES + 1U]; /* FreeRTOS needs +1 */
static StreamBufferHandle_t rxStream;

static uint8_t           rxByte;            /* HAL receives into this, one byte at a time */
static volatile uint32_t rxOverflows = 0;   /* bytes lost because the buffer was full */
static volatile uint32_t rxErrors      = 0; /* UART errors (overrun, framing, noise) */
static volatile uint32_t lastRxErrCode = 0; /* HAL error code of the most recent error */
static volatile uint32_t rxRearmFails = 0;
static volatile uint32_t rxRestarts   = 0;

/* ---------- UART receive: interrupt context ---------- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        BaseType_t woken = pdFALSE;

        if (xStreamBufferSendFromISR(rxStream, &rxByte, 1U, &woken) != 1U) {
            rxOverflows++;
        }
        if (HAL_UART_Receive_IT(&huart1, &rxByte, 1U) != HAL_OK) {
            rxRearmFails++;     /* the CLI task will restart reception */
        }
        portYIELD_FROM_ISR(woken);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        /* An overrun or framing error stops reception; restart it. */
        (void)HAL_UART_Receive_IT(&huart1, &rxByte, 1U);
    }
}

/* ---------- Helpers ---------- */
/* Parse a float and check that it is within [min, max]. */
static bool parse_float(const char *text, float min, float max, float *out)
{
    char *end;
    const float v = strtof(text, &end);

    if ((end == text) || (*end != '\0') || (v < min) || (v > max)) {
        return false;
    }
    *out = v;
    return true;
}

/* Split a line into space-separated words. Modifies the line in place. */
static int tokenize(char *line, char *argv[], int maxArgs)
{
    int   argc = 0;
    char *p    = line;

    while ((*p != '\0') && (argc < maxArgs)) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        argv[argc] = p;
        argc++;
        while ((*p != ' ') && (*p != '\0')) {
            p++;
        }
        if (*p == ' ') {
            *p = '\0';
            p++;
        }
    }
    return argc;
}

/* ---------- Commands ---------- */
typedef void (*cli_handler_t)(int argc, char *argv[]);

typedef struct {
    const char   *name;
    const char   *usage;
    cli_handler_t handler;
} cli_command_t;

static void cmd_help(int argc, char *argv[]);
static void cmd_stats(int argc, char *argv[]);
static void cmd_log(int argc, char *argv[]);
#if VITAL_USE_SIM_SENSOR
static void cmd_sim(int argc, char *argv[]);
#endif

static const cli_command_t commands[] = {
    { "help",  "help                   list commands",               cmd_help  },
    { "stats", "stats                  show task and data stats",    cmd_stats },
    { "log",   "log <on|off>           periodic output on/off",      cmd_log   },
#if VITAL_USE_SIM_SENSOR
    { "sim",   "sim hr|spo2|finger|perf|noise <value>  set simulator input", cmd_sim },
#endif
#if VITAL_FAULT_INJECTION
    { "fault", "fault hang <sample|process|cli|supervisor>", cmd_fault },
#endif
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    for (size_t i = 0U; i < NUM_COMMANDS; i++) {
        app_log("  %s\r\n", commands[i].usage);
    }
}

static void cmd_stats(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    app_print_stats();
    app_log("rx errors=%lu (last 0x%lx) rearm fails=%lu restarts=%lu overflows=%lu\r\n",
            (unsigned long)rxErrors, (unsigned long)lastRxErrCode,
            (unsigned long)rxRearmFails, (unsigned long)rxRestarts,
            (unsigned long)rxOverflows);
    app_log("CPU clock: %lu Hz\r\n", (unsigned long)SystemCoreClock);
}

static void cmd_log(int argc, char *argv[])
{
    if ((argc == 2) && (strcmp(argv[1], "on") == 0)) {
        app_set_periodic_log(true);
        app_log("periodic log on\r\n");
    } else if ((argc == 2) && (strcmp(argv[1], "off") == 0)) {
        app_set_periodic_log(false);
        app_log("periodic log off\r\n");
    } else {
        app_log("usage: log <on|off>\r\n");
    }
}

#if VITAL_USE_SIM_SENSOR
static void cmd_sim(int argc, char *argv[])
{
    float value;

    if (argc != 3) {
        app_log("usage: sim hr <30-240> | spo2 <0-100> | finger <on|off> | perf <0-5> | noise <0-500>\r\n");
        return;
    }

    if (strcmp(argv[1], "hr") == 0) {
        if (parse_float(argv[2], 30.0f, 240.0f, &value)) {
            sensor_sim_set_hr(value);
            app_log("sim: HR = %.1f BPM\r\n", value);
        } else {
            app_log("error: HR must be a number from 30 to 240\r\n");
        }
    } else if (strcmp(argv[1], "spo2") == 0) {
        if (parse_float(argv[2], 0.0f, 100.0f, &value)) {
            sensor_sim_set_spo2(value);
            app_log("sim: SpO2 = %.1f%%\r\n", value);
        } else {
            app_log("error: SpO2 must be a number from 0 to 100\r\n");
        }
    } else if (strcmp(argv[1], "finger") == 0) {
        if (strcmp(argv[2], "on") == 0) {
            sensor_sim_set_finger_present(true);
            app_log("sim: finger present\r\n");
        } else if (strcmp(argv[2], "off") == 0) {
            sensor_sim_set_finger_present(false);
            app_log("sim: finger removed\r\n");
        } else {
            app_log("error: use 'on' or 'off'\r\n");
        }
    } else if (strcmp(argv[1], "perf") == 0) {
        if (parse_float(argv[2], 0.0f, 5.0f, &value)) {
            sensor_sim_set_perfusion(value / 100.0f);
            app_log("sim: perfusion = %.2f%%\r\n", value);
        } else {
            app_log("error: perfusion must be 0 to 5 (percent)\r\n");
        }
    } else if (strcmp(argv[1], "noise") == 0) {
        if (parse_float(argv[2], 0.0f, 500.0f, &value)) {
            sensor_sim_set_noise(value);
            app_log("sim: noise = %.0f counts\r\n", value);
        } else {
            app_log("error: noise must be 0 to 500\r\n");
        }
    } else {
        app_log("error: unknown sim setting '%s'\r\n", argv[1]);
    }
}
#endif

#if VITAL_FAULT_INJECTION
static void cmd_fault(int argc, char *argv[])
{
    if ((argc == 3) && (strcmp(argv[1], "hang") == 0)) {
        if (wd_inject_hang(argv[2])) {
            app_log("injecting hang in '%s'\r\n", argv[2]);
        } else {
            app_log("error: unknown task '%s'\r\n", argv[2]);
        }
    } else {
        app_log("usage: fault hang <sample|process|cli|supervisor>\r\n");
    }
}
#endif

static void execute(char *line)
{
    char *argv[CLI_MAX_ARGS];
    const int argc = tokenize(line, argv, CLI_MAX_ARGS);

    if (argc == 0) {
        return;
    }
    for (size_t i = 0U; i < NUM_COMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0) {
            commands[i].handler(argc, argv);
            return;
        }
    }
    app_log("unknown command '%s' (try 'help')\r\n", argv[0]);
}

/* ---------- CLI task ---------- */
/* Implements: REQ-COM-05 */
static void cli_task(void *arg)
{
    (void)arg;
    char    line[CLI_LINE_MAX];
    size_t  len = 0U;
    uint8_t ch;

    (void)HAL_UART_Receive_IT(&huart1, &rxByte, 1U);
    app_log("\r\nVital CLI ready. Type 'help'.\r\n> ");

    for (;;) {
        wd_checkin(WD_CLI);
        
        /* Self-heal: if reception has stopped, restart it. */
        if (huart1.RxState == HAL_UART_STATE_READY) {
            if (HAL_UART_Receive_IT(&huart1, &rxByte, 1U) == HAL_OK) {
                rxRestarts++;
            }
        }
        if (xStreamBufferReceive(rxStream, &ch, 1U, pdMS_TO_TICKS(100)) != 1U) {
            continue;   /* no key in 100 ms: loop around and check in again */
        }

        if ((ch == '\r') || (ch == '\n')) {
            if (len > 0U) {
                line[len] = '\0';
                app_log("\r\n");
                execute(line);
                len = 0U;
                app_log("> ");
            }
        } else if ((ch == '\b') || (ch == 0x7FU)) {   /* backspace or delete */
            if (len > 0U) {
                len--;
                app_log("\b \b");
            }
        } else if ((ch >= 0x20U) && (ch < 0x7FU)) {   /* printable characters only */
            if (len < (CLI_LINE_MAX - 1U)) {
                line[len] = (char)ch;
                len++;
                app_log("%c", ch);                    /* echo */
            }
        } else {
            /* ignore other control characters */
        }
    }
}

/* ---------- Creation (call before the scheduler starts) ---------- */
void cli_create(void)
{
    rxStream = xStreamBufferCreateStatic(CLI_RX_BUFFER_BYTES, 1U,
                                         rxStreamStorage, &rxStreamBuf);
    configASSERT(rxStream != NULL);

    const TaskHandle_t h = xTaskCreateStatic(cli_task, "cli", CLI_TASK_STACK_WORDS, NULL,
                                             CLI_TASK_PRIORITY, cliTaskStack, &cliTaskTcb);
    configASSERT(h != NULL);
}