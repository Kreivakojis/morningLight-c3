#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "animation_engine.h"
#include "config_manager.h"
#include "led_controller.h"

static const char *TAG = "anim_engine";

// Forward declarations for sunrise_engine (to avoid circular dependency)
typedef enum {
    SUNRISE_STATE_IDLE,
    SUNRISE_STATE_SCHEDULED,
    SUNRISE_STATE_ACTIVE,
    SUNRISE_STATE_COMPLETE,
    SUNRISE_STATE_CANCELLED,
} sunrise_state_t;
extern sunrise_state_t sunrise_engine_get_state(void);
extern void sunrise_engine_cancel(void);

// Forward declarations for time_manager (dark mode check)
extern bool time_manager_is_synced(void);
extern int time_manager_get_day_of_week(void);
extern int time_manager_get_hour(void);
extern int time_manager_get_minute(void);

// External functions
extern void color_utils_kelvin_to_rgb(uint16_t kelvin, uint8_t *r, uint8_t *g, uint8_t *b);
extern void wave_generator_compute(uint8_t *brightness_out, uint16_t led_count,
                                   const animation_preset_t *preset, float time_sec,
                                   const wave_shape_t *shape);

#ifndef CONFIG_ML_ANIM_TASK_STACK_SIZE
#define CONFIG_ML_ANIM_TASK_STACK_SIZE 4096
#endif

// Maximum LED count supported
#define MAX_LED_COUNT 300

// Animation state
static struct {
    bool initialized;
    bool running;
    int8_t active_preset;
    TaskHandle_t task;
    uint8_t *brightness_buffer;
    uint16_t led_count;
    float time_offset;
} anim_state = {0};

static void animation_task(void *arg)
{
    device_config_t *config = config_manager_get();
    animation_preset_t *preset = &config->animation_presets[anim_state.active_preset];
    uint32_t last_tick = xTaskGetTickCount();
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(TAG, "Animation started: preset=%d '%s' wavelength=%.1f speed=%.2f",
             anim_state.active_preset, preset->name, preset->wavelength, preset->speed);

    // Set base color from color temperature
    uint8_t r, g, b;
    color_utils_kelvin_to_rgb(preset->color_temp, &r, &g, &b);
    led_controller_set_rgb(r, g, b);

    while (anim_state.running) {
        uint32_t now = xTaskGetTickCount();
        float dt = (now - last_tick) * portTICK_PERIOD_MS / 1000.0f;
        last_tick = now;
        anim_state.time_offset += dt;

        // Get current preset (may have been updated)
        preset = &config->animation_presets[anim_state.active_preset];

        // Resolve waveform shape for this preset (may have been updated)
        wave_shape_t shape = {
            .kind = config->anim_waveform[anim_state.active_preset],
            .k = config->anim_wave_k_x10[anim_state.active_preset] / 10.0f,
            .i = config->anim_wave_i_x10[anim_state.active_preset] / 10.0f,
        };

        // Compute per-LED brightness values
        wave_generator_compute(
            anim_state.brightness_buffer,
            anim_state.led_count,
            preset,
            anim_state.time_offset,
            &shape
        );

        // Apply global brightness limit
        for (uint16_t i = 0; i < anim_state.led_count; i++) {
            if (anim_state.brightness_buffer[i] > config->brightness_max) {
                anim_state.brightness_buffer[i] = config->brightness_max;
            }
        }

        // Apply to LEDs
        led_controller_set_pixel_brightnesses(
            anim_state.brightness_buffer,
            anim_state.led_count
        );

        // Read fps each iteration so a live setting change takes effect
        // immediately; vTaskDelayUntil locks to a true cadence instead of
        // drifting by however long the compute+SPI-transmit above took.
        uint8_t fps = config->anim_fps >= 10 ? config->anim_fps : 30;
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000 / fps));
    }

    // Clean up
    led_controller_off();
    ESP_LOGI(TAG, "Animation task ended");
    anim_state.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t animation_engine_init(void)
{
    ESP_LOGI(TAG, "Initializing animation engine");

    if (anim_state.initialized) {
        return ESP_OK;
    }

    // Allocate brightness buffer
    anim_state.brightness_buffer = (uint8_t *)malloc(MAX_LED_COUNT);
    if (anim_state.brightness_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate brightness buffer");
        return ESP_ERR_NO_MEM;
    }

    anim_state.led_count = led_controller_get_led_count();
    if (anim_state.led_count > MAX_LED_COUNT) {
        anim_state.led_count = MAX_LED_COUNT;
    }

    anim_state.active_preset = -1;
    anim_state.running = false;
    anim_state.initialized = true;

    ESP_LOGI(TAG, "Animation engine initialized: %d LEDs", anim_state.led_count);
    return ESP_OK;
}

esp_err_t animation_engine_start(uint8_t preset_id)
{
    if (!anim_state.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (preset_id >= ANIMATION_MAX_PRESETS) {
        ESP_LOGE(TAG, "Invalid preset ID: %d", preset_id);
        return ESP_ERR_INVALID_ARG;
    }

    // Check dark mode
    if (time_manager_is_synced()) {
        int d = time_manager_get_day_of_week();
        int h = time_manager_get_hour();
        int m = time_manager_get_minute();
        if (d >= 0 && config_manager_is_dark_mode_blocking(d, h, m)) {
            ESP_LOGW(TAG, "Dark mode active, cannot start animation");
            return ESP_ERR_NOT_ALLOWED;
        }
    }

    // Stop any running sunrise
    if (sunrise_engine_get_state() == SUNRISE_STATE_ACTIVE) {
        ESP_LOGI(TAG, "Stopping sunrise for animation");
        sunrise_engine_cancel();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Stop existing animation
    if (anim_state.running) {
        animation_engine_stop();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Update LED count in case it changed
    anim_state.led_count = led_controller_get_led_count();
    if (anim_state.led_count > MAX_LED_COUNT) {
        anim_state.led_count = MAX_LED_COUNT;
    }

    anim_state.active_preset = preset_id;
    anim_state.time_offset = 0.0f;
    anim_state.running = true;

    // Update config's active animation
    device_config_t *config = config_manager_get();
    config->active_animation = preset_id;

    // Create animation task
    BaseType_t ret = xTaskCreate(
        animation_task,
        "animation",
        CONFIG_ML_ANIM_TASK_STACK_SIZE,
        NULL,
        5,
        &anim_state.task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create animation task");
        anim_state.running = false;
        anim_state.active_preset = -1;
        config->active_animation = -1;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void animation_engine_stop(void)
{
    if (!anim_state.running) {
        return;
    }

    ESP_LOGI(TAG, "Stopping animation");
    anim_state.running = false;

    // Update config
    device_config_t *config = config_manager_get();
    config->active_animation = -1;

    // Wait for task to exit
    if (anim_state.task != NULL) {
        for (int i = 0; i < 20 && anim_state.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    anim_state.active_preset = -1;
}

bool animation_engine_is_running(void)
{
    return anim_state.running;
}

int8_t animation_engine_get_active_preset(void)
{
    return anim_state.active_preset;
}

animation_preset_t *animation_engine_get_preset(uint8_t id)
{
    if (id >= ANIMATION_MAX_PRESETS) {
        return NULL;
    }

    device_config_t *config = config_manager_get();
    return &config->animation_presets[id];
}

esp_err_t animation_engine_save_preset(uint8_t id, const animation_preset_t *preset)
{
    if (id >= ANIMATION_MAX_PRESETS || preset == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    device_config_t *config = config_manager_get();
    memcpy(&config->animation_presets[id], preset, sizeof(animation_preset_t));

    // Ensure name is null-terminated
    config->animation_presets[id].name[sizeof(config->animation_presets[id].name) - 1] = '\0';

    return config_manager_save();
}

void animation_engine_deinit(void)
{
    if (!anim_state.initialized) {
        return;
    }

    animation_engine_stop();

    if (anim_state.brightness_buffer) {
        free(anim_state.brightness_buffer);
        anim_state.brightness_buffer = NULL;
    }

    anim_state.initialized = false;
    ESP_LOGI(TAG, "Animation engine deinitialized");
}
