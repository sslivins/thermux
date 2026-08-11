/**
 * @file bus_stats.c
 * @brief Bounded rolling statistics for 1-Wire read attempts
 */

#include "bus_stats.h"
#include <string.h>

void bus_stats_window_reset(bus_stats_window_t *window)
{
    if (window == NULL) {
        return;
    }

    memset(window, 0, sizeof(*window));
}

void bus_stats_window_record(bus_stats_window_t *window, bool failed)
{
    if (window == NULL) {
        return;
    }

    uint32_t word = window->next_index / 32;
    uint32_t mask = 1U << (window->next_index % 32);

    if (window->sample_count == BUS_STATS_WINDOW_SIZE) {
        if ((window->failure_bits[word] & mask) != 0) {
            window->failed_count--;
        }
    } else {
        window->sample_count++;
    }

    if (failed) {
        window->failure_bits[word] |= mask;
        window->failed_count++;
    } else {
        window->failure_bits[word] &= ~mask;
    }

    window->next_index = (window->next_index + 1) % BUS_STATS_WINDOW_SIZE;
}

void bus_stats_window_get(const bus_stats_window_t *window,
                          uint32_t *sample_count,
                          uint32_t *failed_count)
{
    if (window == NULL) {
        return;
    }

    if (sample_count != NULL) {
        *sample_count = window->sample_count;
    }
    if (failed_count != NULL) {
        *failed_count = window->failed_count;
    }
}
