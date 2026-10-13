# Vital: System Requirements

**Version:** 0.1 (draft)
**Status:** In progress

## 1. Purpose and Scope

This document defines the requirements for Vital, a pulse oximeter and heart rate monitor built on an
STM32U545 running FreeRTOS, with an nRF52840 BLE coprocessor running Zephyr.

> **Disclaimer:** Vital  is not a medical device, is not calibrated, and must not be used for diagnosis
> or patient monitoring.

## 2. Reference Standards

The project's process and structure are loosely modeled on the following
standards. Formal compliance is not claimed.

- IEC 62304: Medical device software life cycle processes
- IEC 60601-1-8: Alarm systems in medical electrical equipment
- ISO 80601-2-61: Particular requirements for pulse oximeter equipment

## 3. System Overview

    [MAX30102] --I2C--> [STM32U545: FreeRTOS] --UART (CRC frames)--> [nRF52840: Zephyr] ~~BLE~~> [Phone]
                         sensing, processing,                         BLE GATT services
                         alarms, watchdog

The STM32U545 performs all safety-relevant functions. The nRF52840 handles
wireless communication only, so a failure of the radio side cannot affect
alarm behavior.

## 4. Definitions

| Term | Definition |
|---|---|
| SpO2 | Peripheral oxygen saturation, in percent |
| HR | Heart rate (pulse rate), in beats per minute (BPM) |
| Physiological alarm | Alarm caused by a measured value outside its limits (e.g., low SpO2) |
| Technical alarm | Alarm caused by an equipment problem (e.g., sensor disconnected) |
| Safe state | Alarm output active, fault code logged, processing halted until reset |
| Processed value | The SpO2 or HR value after filtering and averaging |

## 5. Conventions

**ID format:** `REQ-<AREA>-<NN>`

**Verification methods:**

| Code | Method | Meaning |
|---|---|---|
| T | Test | Operate the system under controlled conditions and measure a quantitative result against a pass/fail criterion. |
| D | Demonstration | Operate the system and observe that it behaves as required, without precise measurement. |
| I | Inspection | Examine code, configuration, build output or documents without running the system. |
| A | Analysis | Use calculation, modeling or review of collected data to show the requirement is met. |

## 6. Requirements

### 6.1 Sensing (SNS)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-SNS-01 | The system shall sample the red and IR channels at 100 Hz ±1%. | T |
| REQ-SNS-02 | The system shall verify the sensor's identity at startup before beginning measurement. | T |
| REQ-SNS-03 | The system shall not lose samples during normal operation. | T |

### 6.2 Signal Processing (PRC)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-PRC-01 | The system shall compute HR over the range 30–240 BPM. | T |
| REQ-PRC-02 | The system shall compute SpO2 over the range 70–100%. | T |
| REQ-PRC-03 | The system shall update processed HR and SpO2 values at least once per second. | T |
| REQ-PRC-04 | The system shall mark readings as invalid when signal quality is below a defined threshold. | T |

### 6.3 Alarms (ALM)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-ALM-01 | The system shall activate a physiological alarm within 200 ms of the processed SpO2 value falling below the low limit (default 90%). | T |
| REQ-ALM-02 | The system shall activate a physiological alarm within 200 ms of the processed HR value leaving the configured limits (default 50–120 BPM). | T |
| REQ-ALM-03 | Physiological and technical alarms shall be distinguishable by the user. | D |
| REQ-ALM-04 | The user shall be able to pause the audible alarm for 2 minutes using the button; the visual alarm shall remain active. | T |
| REQ-ALM-05 | Alarm limits shall be configurable via the command interface. | T |

### 6.4 Fault Detection and Handling (FLT)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-FLT-01 | The system shall enter the safe state within 500 ms if any monitored task fails to check in. | T |
| REQ-FLT-02 | The system shall raise a technical alarm within 1 s of detecting sensor disconnection or finger removal. | T |
| REQ-FLT-03 | The system shall enter the safe state on detection of a task stack overflow. | T |
| REQ-FLT-04 | The system shall record the cause of the most recent fault and report it after reset. | T |
| REQ-FLT-05 | The system shall reset via hardware watchdog if the software fails to service it. | T |

### 6.5 User Interface (UI)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-UI-01 | The system shall display current SpO2 and HR, or an indication that readings are invalid. | D |
| REQ-UI-02 | The system shall test the alarm output (visual and audible) at startup. | D |

### 6.6 Communication (COM)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-COM-01 | The STM32 shall send processed measurements to the BLE coprocessor at least once per second. | T |
| REQ-COM-02 | Inter-processor messages shall include a CRC; messages with an invalid CRC shall be discarded. | T |
| REQ-COM-03 | Loss or reset of the BLE coprocessor shall not affect alarm operation. | T |
| REQ-COM-04 | The BLE coprocessor shall expose HR and SpO2 using the standard Bluetooth SIG Heart Rate and Pulse Oximeter services. | D |
| REQ-COM-05 | The system shall provide a UART command interface for diagnostics and configuration. | D |

### 6.7 Power (PWR)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-PWR-01 | The system shall enter a low-power state when no work is pending. | T |
| REQ-PWR-02 | Average current in normal operation shall be below a target set after initial measurement (TBD). | T |

### 6.8 Software Constraints (SW)

| ID | Requirement | Verif. |
|---|---|---|
| REQ-SW-01 | The firmware shall not allocate memory dynamically after the scheduler starts. | I |
| REQ-SW-02 | Each task shall retain at least 25% unused stack under worst-case observed conditions. | A |
| REQ-SW-03 | The firmware shall compile with no warnings using -Wall -Wextra. | I |
| REQ-SW-04 | Application code shall conform to MISRA C:2012, with deviations documented. | A |
| REQ-SW-05 | Peak CPU load shall remain below 50%. | A |

## 7. Traceability

To be completed as implementation proceeds.

| Requirement | Implementation | Test | Status |
|---|---|---|---|
| REQ-SNS-01 | app_tasks.c: sampling_task | Pending | Implemented (sim) |
| REQ-SNS-02 | app_tasks.c: sampling_task, sensor.h | Pending | Interface only |
| REQ-SNS-03 | app_tasks.c: sampling_task (droppedSamples) | Manual: drop=0 over 10 min | Implemented |
| REQ-COM-05 | cli.c: cli_task, command table | Manual: CLI test session (Step 7) | Implemented |
| REQ-FLT-01 | watchdog.c: supervisor_task; fault.c: safe_state_enter | Manual: fault hang cli/sample (Step 11) | Implemented; latency measurement pending |
| REQ-FLT-03 | app_tasks.c: vApplicationStackOverflowHook → safe_state_enter | Manual: undersized stack | Implemented |
| REQ-FLT-04 | fault.c: fault_boot_check, fault_report_last | Manual: boot report after each fault test | Implemented |
| REQ-FLT-05 | watchdog.c: HAL_IWDG_Refresh; IWDG ~1 s | Manual: fault hang supervisor | Implemented |
| REQ-PRC-01 | ppg.c | test_ppg.c: test_REQ_PRC_01_* (6 tests) | Verified (sim) |
| REQ-PRC-02 | ppg.c | test_ppg.c: test_REQ_PRC_02_* (7 tests) | Verified (sim) |
| REQ-PRC-03 | app_tasks.c: processing_task | test_REQ_PRC_03_valid_within_8_seconds; 1 s report | Implemented |
| REQ-PRC-04 | ppg.c: validity checks | test_REQ_PRC_04_* (2 tests) | Verified (sim) |
| REQ-FLT-02 | ppg.c: finger detection | test_REQ_FLT_02_* (2 tests) | Detection verified; alarm pending |

## 8. Revision History

| Version | Date | Changes |
|---|---|---|
| 0.1 | 2026-10-12 | Initial draft |