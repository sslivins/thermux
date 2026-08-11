/**
 * @file test_bus_stats.c
 * @brief Unit tests for bounded rolling bus statistics
 */

#include "unity.h"
#include "bus_stats.h"

void test_bus_stats_warmup_counts_samples(void)
{
    bus_stats_window_t window = {0};
    uint32_t samples;
    uint32_t failures;

    bus_stats_window_record(&window, false);
    bus_stats_window_record(&window, true);
    bus_stats_window_record(&window, false);
    bus_stats_window_get(&window, &samples, &failures);

    TEST_ASSERT_EQUAL_INT(3, samples);
    TEST_ASSERT_EQUAL_INT(1, failures);
}

void test_bus_stats_window_evicts_old_results(void)
{
    bus_stats_window_t window = {0};
    uint32_t samples;
    uint32_t failures;

    bus_stats_window_record(&window, true);
    for (int i = 1; i < BUS_STATS_WINDOW_SIZE; i++) {
        bus_stats_window_record(&window, false);
    }

    bus_stats_window_get(&window, &samples, &failures);
    TEST_ASSERT_EQUAL_INT(BUS_STATS_WINDOW_SIZE, samples);
    TEST_ASSERT_EQUAL_INT(1, failures);

    bus_stats_window_record(&window, false);
    bus_stats_window_get(&window, &samples, &failures);
    TEST_ASSERT_EQUAL_INT(BUS_STATS_WINDOW_SIZE, samples);
    TEST_ASSERT_EQUAL_INT(0, failures);
}

void test_bus_stats_window_tracks_wrapped_failures(void)
{
    bus_stats_window_t window = {0};
    uint32_t samples;
    uint32_t failures;

    for (int i = 0; i < BUS_STATS_WINDOW_SIZE; i++) {
        bus_stats_window_record(&window, false);
    }
    for (int i = 0; i < 25; i++) {
        bus_stats_window_record(&window, true);
    }

    bus_stats_window_get(&window, &samples, &failures);
    TEST_ASSERT_EQUAL_INT(BUS_STATS_WINDOW_SIZE, samples);
    TEST_ASSERT_EQUAL_INT(25, failures);
}

void test_bus_stats_reset_clears_window(void)
{
    bus_stats_window_t window = {0};
    uint32_t samples;
    uint32_t failures;

    bus_stats_window_record(&window, true);
    bus_stats_window_reset(&window);
    bus_stats_window_get(&window, &samples, &failures);

    TEST_ASSERT_EQUAL_INT(0, samples);
    TEST_ASSERT_EQUAL_INT(0, failures);
    TEST_ASSERT_EQUAL_INT(0, window.next_index);
}

void run_bus_stats_tests(void)
{
    RUN_TEST(test_bus_stats_warmup_counts_samples);
    RUN_TEST(test_bus_stats_window_evicts_old_results);
    RUN_TEST(test_bus_stats_window_tracks_wrapped_failures);
    RUN_TEST(test_bus_stats_reset_clears_window);
}
