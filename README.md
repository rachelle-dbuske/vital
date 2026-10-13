# vital

## Task Architecture

All tasks are created with static allocation (no heap use after startup).
Preemptive scheduling: the highest-priority ready task always runs.

| Priority | Task | Trigger | Responsibility | Rationale |
|:---:|---|---|---|---|
| 6 | `supervisor` | Every 250 ms | Checks all task check-ins, kicks the IWDG, enters the safe state on a miss | Must preempt everything so it can detect any hung task, including the alarm task |
| 5 | `alarm` | Notification from `process`, button interrupt, 25 ms tick | Evaluates limits, drives the LED and audio outputs, handles alarm pause | Safety output must never wait behind data processing |
| 4 | `sample` | Every 10 ms (`vTaskDelayUntil`) | Reads the sensor, queues the sample | Strict 100 Hz timing; a late sample distorts the signal |
| 3 | `process` | Sample queue | Filtering, beat detection, HR and SpO2, publishes vitals | Variable workload; the 32-sample queue absorbs timing jitter |
| 2 | `heartbeat` | Every 5 s | Prints stack high-water marks | Diagnostics only |
| 1 | `cli` | UART stream buffer (100 ms timeout) | Parses and runs commands | Human-speed input; lowest urgency |

### Inter-task communication

| Mechanism | From → To | Purpose |
|---|---|---|
| Queue (32 samples) | `sample` → `process` | Decouples strict sampling from variable processing |
| Task notification | `process`, EXTI13 ISR → `alarm` | Immediate wake-up on new vitals or button press |
| Stream buffer | USART1 ISR → `cli` | Passes received bytes out of the interrupt |
| Event group | all monitored tasks → `supervisor` | Watchdog check-ins |
| Mutex | any task → UART | Prevents interleaved log output |