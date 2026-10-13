/*
 * Unit tests for the PPG algorithm (ppg.c), run on a PC.
 * The synthetic model (ppg_model.c) provides input with a known answer.
 * Test names include the requirement they verify.
 */

#include "unity.h"
#include "ppg.h"
#include "ppg_model.h"

#define FS          100U
#define SEED        12345U

static ppg_t       algo;
static ppg_model_t model;

void setUp(void)
{
    ppg_init(&algo, FS);
    ppg_model_init(&model, (float)FS, SEED);
}

void tearDown(void) {}

/* Feed the algorithm from the model for the given time. */
static void run_seconds(float seconds)
{
    const uint32_t samples = (uint32_t)(seconds * (float)FS);
    for (uint32_t i = 0U; i < samples; i++) {
        uint32_t red, ir;
        ppg_model_next(&model, &red, &ir);
        (void)ppg_process(&algo, red, ir);
    }
}

/* Run until the result becomes invalid; return the elapsed time in seconds, or -1. */
static float seconds_until_invalid(float limit_s)
{
    const uint32_t limit = (uint32_t)(limit_s * (float)FS);
    for (uint32_t i = 1U; i <= limit; i++) {
        uint32_t red, ir;
        ppg_model_next(&model, &red, &ir);
        (void)ppg_process(&algo, red, ir);
        if (!ppg_result(&algo)->valid) {
            return (float)i / (float)FS;
        }
    }
    return -1.0f;
}

static void check_hr(float bpm)
{
    model.hr_bpm = bpm;
    run_seconds(15.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, bpm, ppg_result(&algo)->hr_bpm);
}

static void check_spo2(float spo2)
{
    model.spo2 = spo2;
    run_seconds(15.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, spo2, ppg_result(&algo)->spo2);
}

/* ---------- REQ-PRC-01: HR over 30-240 BPM ---------- */
void test_REQ_PRC_01_hr_30(void)  { check_hr(30.0f);  }
void test_REQ_PRC_01_hr_72(void)  { check_hr(72.0f);  }
void test_REQ_PRC_01_hr_120(void) { check_hr(120.0f); }
void test_REQ_PRC_01_hr_180(void) { check_hr(180.0f); }
void test_REQ_PRC_01_hr_240(void) { check_hr(240.0f); }

void test_REQ_PRC_01_hr_tracks_change(void)
{
    run_seconds(10.0f);
    model.hr_bpm = 120.0f;
    run_seconds(10.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 120.0f, ppg_result(&algo)->hr_bpm);
}

/* ---------- REQ-PRC-02: SpO2 over 70-100% ---------- */
void test_REQ_PRC_02_spo2_100(void) { check_spo2(100.0f); }
void test_REQ_PRC_02_spo2_98(void)  { check_spo2(98.0f);  }
void test_REQ_PRC_02_spo2_90(void)  { check_spo2(90.0f);  }
void test_REQ_PRC_02_spo2_85(void)  { check_spo2(85.0f);  }
void test_REQ_PRC_02_spo2_70(void)  { check_spo2(70.0f);  }

/* Safety: a reading below the accurate range must stay valid so the alarm sees it. */
void test_REQ_PRC_02_spo2_below_range_stays_valid(void)
{
    model.spo2 = 60.0f;
    run_seconds(15.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);
    TEST_ASSERT_LESS_THAN_FLOAT(70.0f, ppg_result(&algo)->spo2);
}

void test_REQ_PRC_02_spo2_at_high_hr(void)
{
    model.hr_bpm = 200.0f;
    check_spo2(88.0f);
}

/* ---------- REQ-PRC-03: results available within a few seconds ---------- */
void test_REQ_PRC_03_valid_within_8_seconds(void)
{
    run_seconds(8.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);
}

/* ---------- REQ-PRC-04: weak signal is marked invalid ---------- */
void test_REQ_PRC_04_no_pulse_is_invalid(void)
{
    model.perfusion = 0.0f;   /* finger present, but no pulse */
    run_seconds(15.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->finger_present);
    TEST_ASSERT_FALSE(ppg_result(&algo)->valid);
}

void test_REQ_PRC_04_pulse_lost_becomes_invalid_within_3s(void)
{
    run_seconds(10.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);

    model.perfusion = 0.0f;
    const float t = seconds_until_invalid(5.0f);
    TEST_ASSERT_TRUE(t > 0.0f);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(3.0f, t);
}

/* ---------- REQ-FLT-02: finger removal detected within 1 s ---------- */
void test_REQ_FLT_02_finger_removal_detected_within_1s(void)
{
    run_seconds(10.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);

    model.finger = false;
    const float t = seconds_until_invalid(2.0f);
    TEST_ASSERT_TRUE(t > 0.0f);
    TEST_ASSERT_LESS_OR_EQUAL_FLOAT(1.0f, t);
    TEST_ASSERT_FALSE(ppg_result(&algo)->finger_present);
}

void test_REQ_FLT_02_recovers_after_finger_replaced(void)
{
    run_seconds(10.0f);
    model.finger = false;
    run_seconds(3.0f);
    model.finger = true;
    run_seconds(10.0f);
    TEST_ASSERT_TRUE(ppg_result(&algo)->finger_present);
    TEST_ASSERT_TRUE(ppg_result(&algo)->valid);
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 72.0f, ppg_result(&algo)->hr_bpm);
}

/* ---------- Robustness ---------- */
void test_heavy_noise_still_accurate(void)
{
    model.noise_counts = 100.0f;   /* 5x the default noise */
    check_hr(72.0f);
    TEST_ASSERT_FLOAT_WITHIN(3.0f, 98.0f, ppg_result(&algo)->spo2);
}

void test_beat_count_matches_rate(void)
{
    run_seconds(60.0f);
    /* 72 BPM for 60 s: about 72 beats (allow startup) */
    TEST_ASSERT_UINT32_WITHIN(3U, 72U, ppg_result(&algo)->beat_count);
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_REQ_PRC_01_hr_30);
    RUN_TEST(test_REQ_PRC_01_hr_72);
    RUN_TEST(test_REQ_PRC_01_hr_120);
    RUN_TEST(test_REQ_PRC_01_hr_180);
    RUN_TEST(test_REQ_PRC_01_hr_240);
    RUN_TEST(test_REQ_PRC_01_hr_tracks_change);

    RUN_TEST(test_REQ_PRC_02_spo2_100);
    RUN_TEST(test_REQ_PRC_02_spo2_98);
    RUN_TEST(test_REQ_PRC_02_spo2_90);
    RUN_TEST(test_REQ_PRC_02_spo2_85);
    RUN_TEST(test_REQ_PRC_02_spo2_70);
    RUN_TEST(test_REQ_PRC_02_spo2_below_range_stays_valid);
    RUN_TEST(test_REQ_PRC_02_spo2_at_high_hr);

    RUN_TEST(test_REQ_PRC_03_valid_within_8_seconds);

    RUN_TEST(test_REQ_PRC_04_no_pulse_is_invalid);
    RUN_TEST(test_REQ_PRC_04_pulse_lost_becomes_invalid_within_3s);

    RUN_TEST(test_REQ_FLT_02_finger_removal_detected_within_1s);
    RUN_TEST(test_REQ_FLT_02_recovers_after_finger_replaced);

    RUN_TEST(test_heavy_noise_still_accurate);
    RUN_TEST(test_beat_count_matches_rate);

    return UNITY_END();
}
