#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_event.h"
#include "config_manager.h"

static const char *TAG = "config_mgr";

#define NVS_NAMESPACE "morninglight"
#define NVS_KEY_CONFIG "config"
#define CONFIG_VERSION 1

// Forward declare app events
extern esp_event_base_t APP_EVENTS;
enum {
    APP_EVENT_CONFIG_CHANGED = 16,
    APP_EVENT_CONFIG_SAVED,
};

// Current configuration
static device_config_t config;
static bool initialized = false;

static void set_defaults(void)
{
    memset(&config, 0, sizeof(config));

    // Default timezone
    strncpy(config.timezone, CONFIG_ML_DEFAULT_TIMEZONE, sizeof(config.timezone) - 1);

    // Default brightness
    config.brightness_max = CONFIG_ML_LED_MAX_BRIGHTNESS;

    // Addressable strip
#ifdef CONFIG_ML_LED_WS1_ENABLED_DEFAULT
    config.ws1_enabled = true;
#else
    config.ws1_enabled = false;
#endif
    config.ws1_led_count = CONFIG_ML_LED_COUNT_DEFAULT;

    // Default alarm (disabled)
    config.alarms[0].hour = 7;
    config.alarms[0].minute = 0;
    config.alarms[0].duration_min = CONFIG_ML_SUNRISE_DEFAULT_DURATION_MIN;
    config.alarms[0].days_mask = 0b0111110;  // Mon-Fri
    config.alarms[0].color_temp = 3000;
    config.alarms[0].brightness = 100;
    config.alarms[0].enabled = false;

    // Default animation_preset to -1 (classic) for all alarms
    for (int i = 0; i < 8; i++) {
        config.alarms[i].animation_preset = -1;
    }

    config.setup_complete = false;

    // Default animation presets
    config.active_animation = -1;  // No animation active by default

    // Preset 0: Gentle Wave
    strncpy(config.animation_presets[0].name, "Gentle", 11);
    config.animation_presets[0].wavelength = 30.0f;
    config.animation_presets[0].amplitude = 30;
    config.animation_presets[0].speed = 0.2f;
    config.animation_presets[0].base_brightness = 50;
    config.animation_presets[0].variation = 20;
    config.animation_presets[0].color_temp = 3000;

    // Preset 1: Ocean
    strncpy(config.animation_presets[1].name, "Ocean", 11);
    config.animation_presets[1].wavelength = 50.0f;
    config.animation_presets[1].amplitude = 50;
    config.animation_presets[1].speed = 0.3f;
    config.animation_presets[1].base_brightness = 40;
    config.animation_presets[1].variation = 40;
    config.animation_presets[1].color_temp = 4500;

    // Presets 2-4: Custom presets (user configurable)
    for (int i = 2; i < ANIMATION_MAX_PRESETS; i++) {
        snprintf(config.animation_presets[i].name, 12, "Custom %d", i + 1);
        config.animation_presets[i].wavelength = 20.0f;
        config.animation_presets[i].amplitude = 50;
        config.animation_presets[i].speed = 0.5f;
        config.animation_presets[i].base_brightness = 50;
        config.animation_presets[i].variation = 0;
        config.animation_presets[i].color_temp = 3500;
    }

    // MQTT defaults
    config.mqtt.enabled = false;
    config.mqtt.broker_uri[0] = '\0';
    config.mqtt.username[0] = '\0';
    config.mqtt.password[0] = '\0';
    strncpy(config.mqtt.topic_prefix, "morninglight", sizeof(config.mqtt.topic_prefix) - 1);
    strncpy(config.mqtt.device_name, "MorningLight-Mini", sizeof(config.mqtt.device_name) - 1);

    // Per-alarm brightness curve and color temp range defaults
    for (int i = 0; i < 8; i++) {
        config.alarm_curves[i] = 0;            // logarithmic
        config.alarm_color_temp_start[i] = 0;  // 0 = no ramp (same as end temp)
    }

    // Per-alarm display names
    memset(config.alarm_names, 0, sizeof(config.alarm_names));

    // Gamma correction
    config.gamma_x10 = 22;

    // WS2811 animated-pixel-path temporal dithering
#ifdef CONFIG_ML_LED_DITHER_ENABLED_DEFAULT
    config.dither_enabled = true;
#else
    config.dither_enabled = false;
#endif
    config.dither_freq_hz = CONFIG_ML_LED_DITHER_FREQ_HZ_DEFAULT;
    config.dither_max_brightness_pct = CONFIG_ML_LED_DITHER_MAX_BRIGHTNESS_PCT_DEFAULT;

    // Per-preset waveform shape (Sine, with neutral Hyperbolic AC params
    // ready if the user switches a preset to that waveform)
    for (int i = 0; i < ANIMATION_MAX_PRESETS; i++) {
        config.anim_waveform[i] = 0;      // Sine
        config.anim_wave_k_x10[i] = 100;  // k = 10.0
        config.anim_wave_i_x10[i] = 20;   // i = 2.0 (unity peak)
    }

    // Animation/wave frame rate
    config.anim_fps = CONFIG_ML_ANIM_FPS_DEFAULT;
}

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager");

    if (initialized) {
        return ESP_OK;
    }

    // Set defaults first
    set_defaults();

    // Try to load from NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_OK) {
        size_t stored_size = 0;
        err = nvs_get_blob(handle, NVS_KEY_CONFIG, NULL, &stored_size);
        if (err == ESP_OK && stored_size > 0) {
            size_t read_size = stored_size < sizeof(device_config_t) ? stored_size : sizeof(device_config_t);
            err = nvs_get_blob(handle, NVS_KEY_CONFIG, &config, &read_size);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Loaded config from NVS (stored=%d, struct=%d)", stored_size, sizeof(device_config_t));
                ESP_LOGI(TAG, "Config values: ws1=%d(%d) anim_fps=%d",
                         config.ws1_enabled, config.ws1_led_count, config.anim_fps);
            } else {
                ESP_LOGW(TAG, "Failed to read config: %s, using defaults", esp_err_to_name(err));
                set_defaults();
            }
        } else {
            ESP_LOGW(TAG, "No config blob in NVS, using defaults");
        }

        nvs_close(handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No config in NVS, using defaults");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(TAG, "Config manager initialized, setup_complete=%d", config.setup_complete);

    return ESP_OK;
}

device_config_t *config_manager_get(void)
{
    return &config;
}

esp_err_t config_manager_save(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Saving config to NVS");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_CONFIG, &config, sizeof(device_config_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved successfully");
        esp_event_post(APP_EVENTS, APP_EVENT_CONFIG_SAVED, NULL, 0, 0);
        esp_event_post(APP_EVENTS, APP_EVENT_CONFIG_CHANGED, NULL, 0, 0);
    }

    return err;
}

esp_err_t config_manager_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset requested");

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    set_defaults();
    return ESP_OK;
}

esp_err_t config_manager_set_wifi(const char *ssid, const char *password)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(config.wifi.ssid, ssid, sizeof(config.wifi.ssid) - 1);
    config.wifi.ssid[sizeof(config.wifi.ssid) - 1] = '\0';

    if (password) {
        strncpy(config.wifi.password, password, sizeof(config.wifi.password) - 1);
        config.wifi.password[sizeof(config.wifi.password) - 1] = '\0';
    } else {
        config.wifi.password[0] = '\0';
    }

    config.setup_complete = true;

    return config_manager_save();
}

alarm_config_t *config_manager_get_alarm(uint8_t id)
{
    if (id >= 8) {
        return NULL;
    }
    return &config.alarms[id];
}

bool config_manager_is_dark_mode_blocking(uint8_t day, uint8_t hour, uint8_t minute)
{
    uint16_t now = hour * 60 + minute;

    for (int i = 0; i < DARK_MODE_MAX_SCHEDULES; i++) {
        dark_mode_schedule_t *s = &config.dark_schedules[i];
        if (!s->enabled || s->allow_override) continue;
        if (!(s->days_mask & (1 << day))) continue;

        uint16_t start = s->start_hour * 60 + s->start_minute;
        uint16_t end = s->end_hour * 60 + s->end_minute;

        if (start <= end) {
            if (now >= start && now < end) return true;
        } else {
            // Overnight window (e.g., 22:00-06:00)
            if (now >= start || now < end) return true;
        }
    }
    return false;
}

alarm_config_t *config_manager_get_next_alarm(uint8_t current_day,
                                               uint8_t current_hour,
                                               uint8_t current_minute,
                                               uint8_t *alarm_id)
{
    // Convert current time to minutes since midnight
    uint16_t current_mins = current_hour * 60 + current_minute;

    alarm_config_t *best_alarm = NULL;
    uint8_t best_id = 0;
    int best_delta = INT32_MAX;

    // Check all alarms
    for (uint8_t i = 0; i < 8; i++) {
        alarm_config_t *alarm = &config.alarms[i];

        if (!alarm->enabled) continue;

        uint16_t alarm_mins = alarm->hour * 60 + alarm->minute;

        // Check each day of the week
        for (int day_offset = 0; day_offset < 7; day_offset++) {
            uint8_t check_day = (current_day + day_offset) % 7;

            // Check if alarm is enabled for this day
            if (!(alarm->days_mask & (1 << check_day))) {
                continue;
            }

            // Calculate minutes until alarm
            int delta;
            if (day_offset == 0 && alarm_mins >= current_mins) {
                // Same day, alarm is now or in future
                delta = alarm_mins - current_mins;
            } else if (day_offset > 0) {
                // Future day
                delta = (day_offset * 24 * 60) + alarm_mins - current_mins;
            } else {
                // Alarm already passed today, skip
                continue;
            }

            if (delta < best_delta) {
                best_delta = delta;
                best_alarm = alarm;
                best_id = i;
            }

            break;  // Found nearest occurrence for this alarm
        }
    }

    if (alarm_id) {
        *alarm_id = best_id;
    }

    return best_alarm;
}
