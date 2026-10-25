#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <time.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the time manager
 *
 * Sets up SNTP client and timezone from config.
 *
 * @return ESP_OK on success
 */
esp_err_t time_manager_init(void);

/**
 * @brief Start NTP time synchronization
 *
 * Should be called after WiFi connection is established.
 *
 * @return ESP_OK on success
 */
esp_err_t time_manager_start_sync(void);

/**
 * @brief Force immediate NTP sync
 */
void time_manager_force_sync(void);

/**
 * @brief Check if time has been synchronized
 *
 * @return true if time is synced
 */
bool time_manager_is_synced(void);

/**
 * @brief Get current time
 *
 * @param tm Output time structure
 * @return ESP_OK on success
 */
esp_err_t time_manager_get_time(struct tm *tm);

/**
 * @brief Get formatted time string
 *
 * @param buf Output buffer
 * @param buf_len Buffer length
 * @return ESP_OK on success
 */
esp_err_t time_manager_get_time_str(char *buf, size_t buf_len);

/**
 * @brief Get current day of week
 *
 * @return Day of week (0=Sunday, 6=Saturday), -1 if not synced
 */
int time_manager_get_day_of_week(void);

/**
 * @brief Get current hour
 *
 * @return Hour (0-23), -1 if not synced
 */
int time_manager_get_hour(void);

/**
 * @brief Get current minute
 *
 * @return Minute (0-59), -1 if not synced
 */
int time_manager_get_minute(void);

/**
 * @brief Set timezone
 *
 * @param tz POSIX timezone string (e.g., "EST5EDT,M3.2.0,M11.1.0")
 * @return ESP_OK on success
 */
esp_err_t time_manager_set_timezone(const char *tz);

/**
 * @brief Get minutes until specified time
 *
 * @param target_hour Target hour (0-23)
 * @param target_minute Target minute (0-59)
 * @param target_day Target day of week (0-6), -1 for any day
 * @return Minutes until target time, -1 on error
 */
int time_manager_minutes_until(uint8_t target_hour, uint8_t target_minute, int target_day);

/**
 * @brief Deinitialize time manager
 */
void time_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // TIME_MANAGER_H
