#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "animation_preset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DARK_MODE_MAX_SCHEDULES 3

/**
 * @brief WiFi credentials storage
 */
typedef struct {
    char ssid[33];
    char password[65];
} wifi_config_stored_t;

/**
 * @brief Single alarm configuration
 */
typedef struct {
    bool enabled;
    uint8_t hour;           // 0-23
    uint8_t minute;         // 0-59
    uint8_t duration_min;   // Sunrise duration in minutes
    uint8_t days_mask;      // Bit mask: bit 0 = Sunday, bit 6 = Saturday
    uint16_t color_temp;    // Color temperature in Kelvin
    uint8_t brightness;     // Max brightness percentage
    int8_t animation_preset; // -1 = classic, 0-4 = animation preset index
    uint8_t cooldown_min;    // 0 = no auto-off, 1-60 = dim-to-off duration
} alarm_config_t;

/**
 * @brief Dark mode schedule
 */
typedef struct {
    bool enabled;
    uint8_t start_hour;     // 0-23
    uint8_t start_minute;   // 0-59
    uint8_t end_hour;       // 0-23
    uint8_t end_minute;     // 0-59
    uint8_t days_mask;      // Bit mask: bit 0 = Sunday, bit 6 = Saturday
    bool allow_override;    // When true, this schedule doesn't block lights
} dark_mode_schedule_t;

/**
 * @brief MQTT configuration
 */
typedef struct {
    bool enabled;
    char broker_uri[128];   // "mqtt://192.168.1.100:1883"
    char username[33];
    char password[65];
    char topic_prefix[32];  // default "morninglight"
    char device_name[32];   // default "MorningLight-Mini" (for HA display)
} mqtt_config_t;

/**
 * @brief Device configuration structure
 */
typedef struct {
    wifi_config_stored_t wifi;
    char timezone[64];
    alarm_config_t alarms[8];
    uint8_t brightness_max;
    bool setup_complete;

    // Animation presets
    animation_preset_t animation_presets[ANIMATION_MAX_PRESETS];
    int8_t active_animation;    // -1 = none, 0-4 = preset index

    // Dark mode schedules
    dark_mode_schedule_t dark_schedules[DARK_MODE_MAX_SCHEDULES];

    // MQTT configuration
    mqtt_config_t mqtt;

    // Per-alarm brightness curve and color temp range
    uint8_t alarm_curves[8];            // 0=logarithmic(default), 1=inverse_log, 2=linear, 3=sigmoid, 4=exponential
    uint16_t alarm_color_temp_start[8]; // Starting color temp in K, 0 = same as alarm's color_temp (no ramp)

    // Per-alarm display names
    char alarm_names[8][16];            // Optional display name, empty string = unnamed

    // Gamma correction
    uint8_t gamma_x10;                  // Gamma * 10: 22 = 2.2. Range 10-40 (1.0-4.0). 0 treated as 22

    // Single addressable strip
    bool ws1_enabled;                   // Addressable strip populated
    uint16_t ws1_led_count;             // Strip LED count (1-300)

    // WS2811 animated-pixel-path temporal dithering
    bool dither_enabled;                // Error-diffusion dithering on the animation/wave pixel path
    uint8_t dither_freq_hz;             // How often the dither step updates, 1-60 Hz
    uint8_t dither_max_brightness_pct;  // Dithering only applies at or below this brightness, 0-100

    // Per-preset waveform shape. Kept as parallel arrays rather than fields
    // on animation_preset_t, because that struct is embedded mid-struct
    // (animation_presets[]) above.
    uint8_t anim_waveform[ANIMATION_MAX_PRESETS];   // 0 = Sine, 1 = Hyperbolic AC, 2 = Hyperbolic Pulse
    uint8_t anim_wave_k_x10[ANIMATION_MAX_PRESETS]; // Zero-crossing sharpness * 10, range 5-200 (k: 0.5-20.0)
    uint8_t anim_wave_i_x10[ANIMATION_MAX_PRESETS]; // Crest shape * 10, range 2-40 (i: 0.2-4.0; i=2.0 -> unity peak)

    // Animation/wave frame rate
    uint8_t anim_fps;                   // Frame rate for the wave animation pixel path: 30 or 60
} device_config_t;

/**
 * @brief Initialize the configuration manager
 *
 * Loads configuration from NVS or creates defaults.
 *
 * @return ESP_OK on success
 */
esp_err_t config_manager_init(void);

/**
 * @brief Get pointer to current configuration
 *
 * @return Pointer to config structure (do not free)
 */
device_config_t *config_manager_get(void);

/**
 * @brief Save current configuration to NVS
 *
 * @return ESP_OK on success
 */
esp_err_t config_manager_save(void);

/**
 * @brief Reset configuration to factory defaults
 *
 * @return ESP_OK on success
 */
esp_err_t config_manager_factory_reset(void);

/**
 * @brief Set WiFi credentials
 *
 * @param ssid Network SSID
 * @param password Network password
 * @return ESP_OK on success
 */
esp_err_t config_manager_set_wifi(const char *ssid, const char *password);

/**
 * @brief Get alarm by ID
 *
 * @param id Alarm ID (0-7)
 * @return Pointer to alarm config, NULL if invalid ID
 */
alarm_config_t *config_manager_get_alarm(uint8_t id);

/**
 * @brief Get next enabled alarm
 *
 * @param current_day Current day of week (0=Sunday)
 * @param current_hour Current hour
 * @param current_minute Current minute
 * @param alarm_id Output: ID of next alarm
 * @return Pointer to next alarm, NULL if none
 */
alarm_config_t *config_manager_get_next_alarm(uint8_t current_day,
                                               uint8_t current_hour,
                                               uint8_t current_minute,
                                               uint8_t *alarm_id);

/**
 * @brief Check if dark mode is currently blocking lights
 *
 * @param day Day of week (0=Sunday)
 * @param hour Current hour (0-23)
 * @param minute Current minute (0-59)
 * @return true if dark mode is active and blocking
 */
bool config_manager_is_dark_mode_blocking(uint8_t day, uint8_t hour, uint8_t minute);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H
