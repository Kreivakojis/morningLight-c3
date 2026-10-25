#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "time_manager.h"
#include "config_manager.h"

static const char *TAG = "time_mgr";

// Forward declare app events
extern esp_event_base_t APP_EVENTS;
enum {
    APP_EVENT_TIME_SYNCED = 5,
    APP_EVENT_TIME_SYNC_FAILED,
};

// State
static struct {
    bool initialized;
    bool synced;
    char timezone[64];
} state = {0};

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized");
    state.synced = true;

    // Log current time
    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Current time: %s %s", strftime_buf, state.timezone);

    esp_event_post(APP_EVENTS, APP_EVENT_TIME_SYNCED, NULL, 0, 0);
}

esp_err_t time_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing time manager");

    if (state.initialized) {
        return ESP_OK;
    }

    // Get timezone from config
    device_config_t *config = config_manager_get();
    if (config && strlen(config->timezone) > 0) {
        strncpy(state.timezone, config->timezone, sizeof(state.timezone) - 1);
    } else {
        strncpy(state.timezone, CONFIG_ML_DEFAULT_TIMEZONE, sizeof(state.timezone) - 1);
    }

    // Set timezone
    setenv("TZ", state.timezone, 1);
    tzset();

    ESP_LOGI(TAG, "Timezone set to: %s", state.timezone);

    state.initialized = true;
    state.synced = false;

    return ESP_OK;
}

esp_err_t time_manager_start_sync(void)
{
    if (!state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting NTP sync");

    // Configure SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_ML_NTP_SERVER1);
    esp_sntp_setservername(1, CONFIG_ML_NTP_SERVER2);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);

    // Set sync interval (in seconds)
    esp_sntp_set_sync_interval(CONFIG_ML_NTP_SYNC_INTERVAL_H * 3600 * 1000);

    esp_sntp_init();

    return ESP_OK;
}

void time_manager_force_sync(void)
{
    if (!state.initialized) return;

    ESP_LOGI(TAG, "Forcing NTP sync");

    // Restart SNTP to force resync
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_init();
}

bool time_manager_is_synced(void)
{
    return state.synced;
}

esp_err_t time_manager_get_time(struct tm *tm)
{
    if (tm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t now = time(NULL);
    localtime_r(&now, tm);

    return ESP_OK;
}

esp_err_t time_manager_get_time_str(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 6) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!state.synced) {
        strncpy(buf, "--:--", buf_len);
        return ESP_ERR_INVALID_STATE;
    }

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    strftime(buf, buf_len, "%H:%M", &timeinfo);

    return ESP_OK;
}

int time_manager_get_day_of_week(void)
{
    if (!state.synced) return -1;

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    return timeinfo.tm_wday;
}

int time_manager_get_hour(void)
{
    if (!state.synced) return -1;

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    return timeinfo.tm_hour;
}

int time_manager_get_minute(void)
{
    if (!state.synced) return -1;

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    return timeinfo.tm_min;
}

esp_err_t time_manager_set_timezone(const char *tz)
{
    if (tz == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(state.timezone, tz, sizeof(state.timezone) - 1);
    state.timezone[sizeof(state.timezone) - 1] = '\0';

    setenv("TZ", state.timezone, 1);
    tzset();

    ESP_LOGI(TAG, "Timezone changed to: %s", state.timezone);

    return ESP_OK;
}

int time_manager_minutes_until(uint8_t target_hour, uint8_t target_minute, int target_day)
{
    if (!state.synced) return -1;

    struct tm timeinfo;
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    int current_mins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int target_mins = target_hour * 60 + target_minute;

    if (target_day < 0) {
        // Any day - just calculate time until next occurrence
        if (target_mins >= current_mins) {
            return target_mins - current_mins;
        } else {
            // Tomorrow
            return (24 * 60) - current_mins + target_mins;
        }
    }

    // Specific day
    int days_until = target_day - timeinfo.tm_wday;
    if (days_until < 0) {
        days_until += 7;
    } else if (days_until == 0 && target_mins <= current_mins) {
        days_until = 7;  // Next week
    }

    return (days_until * 24 * 60) + target_mins - current_mins;
}

void time_manager_deinit(void)
{
    if (!state.initialized) return;

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    state.initialized = false;
    state.synced = false;
}
