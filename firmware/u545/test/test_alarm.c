/*
 * Unit tests for the alarm logic (alarm.c), run on a PC.
 * Most tests feed hand-made readings; the last ones run the full chain
 * model -> ppg algorithm -> alarm.
 */

#include "unity.h"
#include "alarm.h"
#include "ppg.h"
#include "ppg_model.h"

static alarm_t        alm;
static alarm_output_t out;
static uint32_t       now;

void setUp(void)
{
    alarm_init(&alm);
    now = 1000U;
}

void tearDown(void) {}

static ppg_result_t reading(float hr, float spo2)
{
    ppg_result_t r = { .finger_present = true, .valid = true,
                       .hr_bpm = hr, .spo2 = spo2, .perfusion = 0.01f, .beat_count = 0U };
    return r;
}

static ppg_result_t no_finger(void)
{
    ppg_result_t r = { .finger_present = false, .valid = false };
    return r;
}

static ppg_result_t invalid(void)
{
    ppg_result_t r = { .finger_present = true, .valid = false };
    return r;
}

/* Feed the same reading every 50 ms for the given time. */
static void hold(ppg_result_t r, uint32_t ms)
{
    /* Count elapsed time, not "now < end": that comparison breaks when the
     * millisecond counter wraps around. */
    for (uint32_t elapsed = 0U; elapsed < ms; elapsed += 50U) {
        alarm_update(&alm, &r, now, &out);
        now += 50U;
    }
}

static void step(ppg_result_t r)
{
    alarm_update(&alm, &r, now, &out);
}

/* ---------- Normal operation ---------- */
void test_normal_reading_no_alarm(void)
{
    step(reading(72.0f, 98.0f));
    TEST_ASSERT_EQUAL_UINT32(0U, out.conditions);
    TEST_ASSERT_FALSE(out.audible);
    TEST_ASSERT_FALSE(out.led_on);
}

/* ---------- REQ-ALM-01: low SpO2 ---------- */
void test_REQ_ALM_01_low_spo2_alarms_on_first_reading(void)
{
    step(reading(72.0f, 98.0f));
    step(reading(72.0f, 89.9f));     /* first processed value below 90 */
    TEST_ASSERT_TRUE(out.physiological);
    TEST_ASSERT_BITS_HIGH(ALM_SPO2_LOW, out.conditions);
    TEST_ASSERT_TRUE(out.audible);
}

void test_REQ_ALM_01_at_limit_no_alarm(void)
{
    step(reading(72.0f, 90.0f));     /* "below 90%": exactly 90 is not an alarm */
    TEST_ASSERT_FALSE(out.physiological);
}

void test_REQ_ALM_01_hysteresis_prevents_chatter(void)
{
    step(reading(72.0f, 89.0f));
    TEST_ASSERT_TRUE(out.physiological);
    step(reading(72.0f, 90.5f));     /* back above 90, but not by the margin */
    TEST_ASSERT_TRUE(out.physiological);
    step(reading(72.0f, 91.0f));     /* clearly recovered */
    TEST_ASSERT_FALSE(out.physiological);
}

void test_REQ_ALM_01_very_low_spo2_alarms(void)
{
    step(reading(72.0f, 55.0f));
    TEST_ASSERT_BITS_HIGH(ALM_SPO2_LOW, out.conditions);
}

/* ---------- REQ-ALM-02: HR limits ---------- */
void test_REQ_ALM_02_low_hr(void)
{
    step(reading(45.0f, 98.0f));
    TEST_ASSERT_BITS_HIGH(ALM_HR_LOW, out.conditions);
}

void test_REQ_ALM_02_high_hr(void)
{
    step(reading(130.0f, 98.0f));
    TEST_ASSERT_BITS_HIGH(ALM_HR_HIGH, out.conditions);
}

void test_REQ_ALM_02_high_hr_clears_with_hysteresis(void)
{
    step(reading(125.0f, 98.0f));
    step(reading(119.0f, 98.0f));    /* within 2 BPM of the limit: hold */
    TEST_ASSERT_BITS_HIGH(ALM_HR_HIGH, out.conditions);
    step(reading(117.0f, 98.0f));
    TEST_ASSERT_BITS_LOW(ALM_HR_HIGH, out.conditions);
}

/* ---------- REQ-ALM-03: distinguishable alarms ---------- */
void test_REQ_ALM_03_physiological_blinks_fast(void)
{
    uint32_t toggles = 0U;
    bool last = false;
    for (uint32_t i = 0U; i < 40U; i++) {     /* 1 s in 25 ms steps */
        ppg_result_t r = reading(72.0f, 85.0f);
        alarm_update(&alm, &r, now, &out);
        if (out.led_on != last) { toggles++; last = out.led_on; }
        now += 25U;
    }
    TEST_ASSERT_TRUE(toggles >= 7U);          /* ~4 Hz */
}

void test_REQ_ALM_03_technical_blinks_slow(void)
{
    uint32_t toggles = 0U;
    bool last = false;
    for (uint32_t i = 0U; i < 40U; i++) {
        ppg_result_t r = no_finger();
        alarm_update(&alm, &r, now, &out);
        if (out.led_on != last) { toggles++; last = out.led_on; }
        now += 25U;
    }
    TEST_ASSERT_TRUE(out.technical);
    TEST_ASSERT_FALSE(out.physiological);
    TEST_ASSERT_TRUE(toggles <= 2U);          /* 0.5 Hz */
}

void test_REQ_ALM_03_physiological_has_priority(void)
{
    step(reading(72.0f, 85.0f));
    ppg_result_t r = invalid();
    hold(r, ALM_SIGNAL_LOST_DELAY_MS + 100U); /* both kinds active */
    TEST_ASSERT_TRUE(out.physiological);
    TEST_ASSERT_TRUE(out.technical);
    /* fast-blink pattern is used: LED changes within 125 ms */
    const bool a = out.led_on;
    now += 125U;
    step(invalid());
    TEST_ASSERT_NOT_EQUAL(a, out.led_on);
}

/* ---------- REQ-ALM-04: audio pause ---------- */
void test_REQ_ALM_04_pause_silences_audio_keeps_visual(void)
{
    step(reading(72.0f, 85.0f));
    alarm_button_pressed(&alm, now);
    hold(reading(72.0f, 85.0f), 1000U);
    TEST_ASSERT_TRUE(out.audio_paused);
    TEST_ASSERT_FALSE(out.audible);
    TEST_ASSERT_TRUE(out.physiological);      /* visual alarm still active */
}

void test_REQ_ALM_04_pause_expires_after_2_minutes(void)
{
    step(reading(72.0f, 85.0f));
    alarm_button_pressed(&alm, now);
    hold(reading(72.0f, 85.0f), ALM_PAUSE_MS - 1000U);
    TEST_ASSERT_FALSE(out.audible);
    hold(reading(72.0f, 85.0f), 2000U);
    TEST_ASSERT_TRUE(out.audible);
}

void test_REQ_ALM_04_press_without_alarm_ignored(void)
{
    step(reading(72.0f, 98.0f));
    alarm_button_pressed(&alm, now);
    step(reading(72.0f, 85.0f));              /* new alarm must sound */
    TEST_ASSERT_TRUE(out.audible);
}

void test_REQ_ALM_04_pause_ends_when_alarms_clear(void)
{
    step(reading(72.0f, 85.0f));
    alarm_button_pressed(&alm, now);
    step(reading(72.0f, 98.0f));              /* recovered: pause cancelled */
    step(reading(72.0f, 85.0f));              /* a new event must be audible */
    TEST_ASSERT_TRUE(out.audible);
}

/* ---------- REQ-ALM-05: configurable limits ---------- */
void test_REQ_ALM_05_new_limit_applies(void)
{
    alarm_limits_t l = { .spo2_low = 85.0f, .hr_low = 50.0f, .hr_high = 120.0f };
    TEST_ASSERT_TRUE(alarm_set_limits(&alm, &l));
    step(reading(72.0f, 87.0f));
    TEST_ASSERT_FALSE(out.physiological);
    step(reading(72.0f, 84.0f));
    TEST_ASSERT_TRUE(out.physiological);
}

void test_REQ_ALM_05_unsafe_limits_rejected(void)
{
    alarm_limits_t too_low  = { .spo2_low = 40.0f, .hr_low = 50.0f, .hr_high = 120.0f };
    alarm_limits_t inverted = { .spo2_low = 90.0f, .hr_low = 120.0f, .hr_high = 50.0f };
    alarm_limits_t too_tight = { .spo2_low = 90.0f, .hr_low = 70.0f, .hr_high = 75.0f };
    TEST_ASSERT_FALSE(alarm_set_limits(&alm, &too_low));
    TEST_ASSERT_FALSE(alarm_set_limits(&alm, &inverted));
    TEST_ASSERT_FALSE(alarm_set_limits(&alm, &too_tight));
    TEST_ASSERT_EQUAL_FLOAT(90.0f, alm.limits.spo2_low);   /* unchanged */
}

/* ---------- REQ-FLT-02: technical alarms ---------- */
void test_REQ_FLT_02_no_finger_is_immediate_technical_alarm(void)
{
    step(reading(72.0f, 98.0f));
    step(no_finger());
    TEST_ASSERT_TRUE(out.technical);
    TEST_ASSERT_BITS_HIGH(ALM_NO_FINGER, out.conditions);
    TEST_ASSERT_TRUE(out.audible);
}

void test_REQ_FLT_02_finger_removal_clears_physiological(void)
{
    step(reading(72.0f, 85.0f));
    step(no_finger());
    TEST_ASSERT_FALSE(out.physiological);     /* values are meaningless now */
}

void test_signal_lost_after_delay_only(void)
{
    step(reading(72.0f, 98.0f));
    hold(invalid(), ALM_SIGNAL_LOST_DELAY_MS - 100U);
    TEST_ASSERT_FALSE(out.technical);         /* still acquiring: no alarm yet */
    hold(invalid(), 200U);
    TEST_ASSERT_BITS_HIGH(ALM_SIGNAL_LOST, out.conditions);
}

void test_invalid_signal_holds_physiological_alarm(void)
{
    step(reading(72.0f, 85.0f));
    hold(invalid(), 2000U);
    TEST_ASSERT_TRUE(out.physiological);      /* not declared fine by noise */
}

void test_timer_wraparound(void)
{
    now = 0xFFFFFF00U;                        /* ms counter about to wrap */
    step(reading(72.0f, 85.0f));
    alarm_button_pressed(&alm, now);
    hold(reading(72.0f, 85.0f), 1000U);       /* crosses the wrap */
    TEST_ASSERT_FALSE(out.audible);           /* pause did not end early */
}

/* ---------- End to end: model -> algorithm -> alarm ---------- */
static void run_chain(ppg_model_t *m, ppg_t *p, uint32_t seconds)
{
    for (uint32_t i = 0U; i < (seconds * 100U); i++) {
        uint32_t red, ir;
        ppg_model_next(m, &red, &ir);
        (void)ppg_process(p, red, ir);
        if ((i % 5U) == 0U) {                 /* alarm task runs every 50 ms */
            alarm_update(&alm, ppg_result(p), now, &out);
        }
        now += 10U;
    }
}

void test_end_to_end_spo2_drop_triggers_alarm(void)
{
    ppg_model_t m;
    ppg_t p;
    ppg_model_init(&m, 100.0f, 12345U);
    ppg_init(&p, 100U);

    run_chain(&m, &p, 15U);
    TEST_ASSERT_EQUAL_UINT32(0U, out.conditions);

    m.spo2 = 85.0f;
    run_chain(&m, &p, 10U);
    TEST_ASSERT_BITS_HIGH(ALM_SPO2_LOW, out.conditions);
}

void test_end_to_end_finger_removed_triggers_technical(void)
{
    ppg_model_t m;
    ppg_t p;
    ppg_model_init(&m, 100.0f, 12345U);
    ppg_init(&p, 100U);

    run_chain(&m, &p, 15U);
    m.finger = false;
    run_chain(&m, &p, 1U);
    TEST_ASSERT_BITS_HIGH(ALM_NO_FINGER, out.conditions);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_reading_no_alarm);
    RUN_TEST(test_REQ_ALM_01_low_spo2_alarms_on_first_reading);
    RUN_TEST(test_REQ_ALM_01_at_limit_no_alarm);
    RUN_TEST(test_REQ_ALM_01_hysteresis_prevents_chatter);
    RUN_TEST(test_REQ_ALM_01_very_low_spo2_alarms);
    RUN_TEST(test_REQ_ALM_02_low_hr);
    RUN_TEST(test_REQ_ALM_02_high_hr);
    RUN_TEST(test_REQ_ALM_02_high_hr_clears_with_hysteresis);
    RUN_TEST(test_REQ_ALM_03_physiological_blinks_fast);
    RUN_TEST(test_REQ_ALM_03_technical_blinks_slow);
    RUN_TEST(test_REQ_ALM_03_physiological_has_priority);
    RUN_TEST(test_REQ_ALM_04_pause_silences_audio_keeps_visual);
    RUN_TEST(test_REQ_ALM_04_pause_expires_after_2_minutes);
    RUN_TEST(test_REQ_ALM_04_press_without_alarm_ignored);
    RUN_TEST(test_REQ_ALM_04_pause_ends_when_alarms_clear);
    RUN_TEST(test_REQ_ALM_05_new_limit_applies);
    RUN_TEST(test_REQ_ALM_05_unsafe_limits_rejected);
    RUN_TEST(test_REQ_FLT_02_no_finger_is_immediate_technical_alarm);
    RUN_TEST(test_REQ_FLT_02_finger_removal_clears_physiological);
    RUN_TEST(test_signal_lost_after_delay_only);
    RUN_TEST(test_invalid_signal_holds_physiological_alarm);
    RUN_TEST(test_timer_wraparound);
    RUN_TEST(test_end_to_end_spo2_drop_triggers_alarm);
    RUN_TEST(test_end_to_end_finger_removed_triggers_technical);
    return UNITY_END();
}
