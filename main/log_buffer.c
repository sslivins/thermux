/**
 * @file log_buffer.c
 * @brief Circular buffer for capturing ESP-IDF logs for web display
 */

#include "log_buffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define DEFAULT_BUFFER_SIZE 4096

static char *s_buffer = NULL;
static size_t s_buffer_size = 0;
static size_t s_head = 0;  /* Write position */
static size_t s_used = 0;  /* Bytes used */
static SemaphoreHandle_t s_mutex = NULL;
static vprintf_like_t s_original_vprintf = NULL;

/**
 * @brief Custom vprintf that writes to both serial and ring buffer
 */
static int log_vprintf(const char *fmt, va_list args)
{
    /* First, call original to output to serial */
    int ret = 0;
    if (s_original_vprintf) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_original_vprintf(fmt, args_copy);
        va_end(args_copy);
    }
    
    /* Then write to ring buffer - use static buffer to avoid stack overflow */
    if (s_buffer && s_mutex) {
        static char temp[128];  /* Static to avoid stack issues in small-stack tasks */
        static SemaphoreHandle_t print_mutex = NULL;
        
        /* Create print mutex on first use */
        if (!print_mutex) {
            print_mutex = xSemaphoreCreateMutex();
            if (!print_mutex) return ret;
        }
        
        /* Serialize access to static temp buffer */
        if (xSemaphoreTake(print_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
            return ret;
        }
        
        int len = vsnprintf(temp, sizeof(temp), fmt, args);
        if (len > 0) {
            if (len >= sizeof(temp)) {
                len = sizeof(temp) - 1;
            }
            
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                for (int i = 0; i < len; i++) {
                    s_buffer[s_head] = temp[i];
                    s_head = (s_head + 1) % s_buffer_size;
                    if (s_used < s_buffer_size) {
                        s_used++;
                    }
                }
                xSemaphoreGive(s_mutex);
            }
        }
        
        xSemaphoreGive(print_mutex);
    }
    
    return ret;
}

esp_err_t log_buffer_init(size_t buffer_size)
{
    if (buffer_size == 0) {
        buffer_size = DEFAULT_BUFFER_SIZE;
    }
    
    s_buffer = malloc(buffer_size);
    if (!s_buffer) {
        return ESP_ERR_NO_MEM;
    }
    
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        free(s_buffer);
        s_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    s_buffer_size = buffer_size;
    s_head = 0;
    s_used = 0;
    
    /* Hook into ESP logging */
    s_original_vprintf = esp_log_set_vprintf(log_vprintf);
    
    return ESP_OK;
}

size_t log_buffer_get(char *out_buffer, size_t buffer_size)
{
    if (!s_buffer || !s_mutex || !out_buffer || buffer_size == 0) {
        return 0;
    }
    
    size_t copied = 0;
    
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_used > 0) {
            /* Calculate start position (oldest data) */
            size_t start;
            size_t to_copy = s_used;
            
            if (s_used >= s_buffer_size) {
                /* Buffer is full, start is at head */
                start = s_head;
            } else {
                /* Buffer not full, start is at beginning */
                start = (s_head + s_buffer_size - s_used) % s_buffer_size;
            }
            
            /* Limit to output buffer size (leave room for null) */
            if (to_copy >= buffer_size) {
                /* Skip oldest data to fit */
                size_t skip = to_copy - buffer_size + 1;
                start = (start + skip) % s_buffer_size;
                to_copy = buffer_size - 1;
            }
            
            /* Copy from ring buffer to output */
            for (size_t i = 0; i < to_copy; i++) {
                out_buffer[copied++] = s_buffer[(start + i) % s_buffer_size];
            }
        }
        xSemaphoreGive(s_mutex);
    }
    
    out_buffer[copied] = '\0';
    return copied;
}

void log_buffer_clear(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_head = 0;
        s_used = 0;
        xSemaphoreGive(s_mutex);
    }
}

void log_buffer_get_info(size_t *used, size_t *total)
{
    if (used) *used = s_used;
    if (total) *total = s_buffer_size;
}
