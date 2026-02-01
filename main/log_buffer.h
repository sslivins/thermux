/**
 * @file log_buffer.h
 * @brief Circular buffer for capturing ESP-IDF logs for web display
 */

#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include "esp_err.h"
#include <stddef.h>

/** @brief Default log buffer size (16KB) */
#define LOG_BUFFER_SIZE 16384

/**
 * @brief Initialize log buffer and hook into ESP logging
 * @param buffer_size Size of circular buffer in bytes (default LOG_BUFFER_SIZE if 0)
 */
esp_err_t log_buffer_init(size_t buffer_size);

/**
 * @brief Get current log contents
 * @param out_buffer Buffer to copy logs into
 * @param buffer_size Size of output buffer
 * @return Number of bytes copied
 */
size_t log_buffer_get(char *out_buffer, size_t buffer_size);

/**
 * @brief Clear the log buffer
 */
void log_buffer_clear(void);

/**
 * @brief Get buffer usage info
 * @param used Pointer to store bytes used
 * @param total Pointer to store total size
 */
void log_buffer_get_info(size_t *used, size_t *total);

#endif /* LOG_BUFFER_H */
