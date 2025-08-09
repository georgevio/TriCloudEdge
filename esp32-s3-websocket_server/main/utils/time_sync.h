#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initialize the SNTP service and wait for time to be synchronized.
 *
 * Blocking function that should be called once after a network
 * connection is established. On success, it populates the provided buffer
 * with a human-readable time string.
 *
 * @param time_str_buf Buffer to store the formatted time string on success.
 * @param buf_len Size of the buffer.
 * @return ESP_OK on success, ESP_FAIL on failure/timeout.
 */
esp_err_t time_sync_init(char* time_str_buf, size_t buf_len);

/**
 * @brief Checks if the system time has been synchronized.
 *
 * @return true if time is synchronized, false otherwise.
 */
bool is_time_synchronized(void);

/**
 * @brief Gets the current time as a human-readable string 
 * (e.g., "Thursday, July 10, 2025 09:55:34").
 *
 * @param buf Buffer to store the formatted time string.
 * @param buf_len Size of the buffer.
 */
void get_human_readable_time_string(char* buf, size_t buf_len);

/**
 * @brief Gets the current UTC time as an ISO 8601 formatted string 
 * (e.g., "2025-07-10T07:55:34Z").
 * This function requires that the time has already been synchronized.
 * This format is for logs.
 *
 * @param buf Buffer to store the formatted time string.
 * @param buf_len Size of the buffer.
 */
void get_utc_time_string(char* buf, size_t buf_len);

#endif // TIME_SYNC_H