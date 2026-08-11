/**
 * @file bus_stats.h
 * @brief Bounded rolling statistics for 1-Wire read attempts
 */

#ifndef BUS_STATS_H
#define BUS_STATS_H

#include <stdbool.h>
#include <stdint.h>

#define BUS_STATS_WINDOW_SIZE 1000
#define BUS_STATS_WORD_COUNT ((BUS_STATS_WINDOW_SIZE + 31) / 32)

typedef struct {
    uint32_t failure_bits[BUS_STATS_WORD_COUNT];
    uint32_t sample_count;
    uint32_t failed_count;
    uint32_t next_index;
} bus_stats_window_t;

void bus_stats_window_reset(bus_stats_window_t *window);
void bus_stats_window_record(bus_stats_window_t *window, bool failed);
void bus_stats_window_get(const bus_stats_window_t *window,
                          uint32_t *sample_count,
                          uint32_t *failed_count);

#endif /* BUS_STATS_H */
