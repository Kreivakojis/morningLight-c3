#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "sunrise_engine.h"
#include "config_manager.h"
#include "animation_preset.h"
#include "time_manager.h"
#include "led_controller.h"

// Forward declarations for animation_engine (to avoid circular dependency)
extern bool animation_engine_is_running(void);
extern void animation_engine_stop(void);

// External curve functions
extern float sunrise_curve_apply(sunrise_curve_t curve, float t);
extern uint16_t sunrise_color_interpolate(float progress, uint16_t start_temp, uint16_t end_temp);

// External wave generator function for animated sunrise
extern void wave_generator_compute_with_base(uint8_t *, uint16_t, const animation_preset_t *, float, int16_t, float, const wave_shape_t *);

// External color utility function
extern void color_utils_kelvin_to_rgb(uint16_t kelvin, uint8_t *r, uint8_t *g, uint8_t *b);

static const char *TAG = "sunrise";

// Forward declare app events
extern esp_event_base_t APP_EVENTS;
enum {
    APP_EVENT_ALARM_TRIGGERED = 7,
    APP_EVENT_ALARM_COMPLETE,
    APP_EVENT_ALARM_CANCELLED,
};

typedef struct {
    uint8_t alarm_id;
    uint32_t progress_percent;
} app_event_alarm_t;

// State
static struct {
    bool initialized;
    sunrise_state_t state;
    sunrise_curve_t curve;

    // Scheduled alarm info
    int8_t scheduled_alarm_id;
    int minutes_until_alarm;

    // Active sunrise info
    uint32_t start_time_ms;
    uint32_t duration_ms;
    uint16_t color_temp_start;
    uint16_t color_temp_end;
    uint8_t max_brightness;
    float current_progress;

    // Animation mode fields
    bool animation_mode;
    int8_t animation_preset_id;
    animation_preset_t preset_copy;    // Local copy to avoid race conditions
    uint8_t *brightness_buffer;        // Per-LED brightness array
    uint16_t led_count;

    // Cooldown fields
    uint8_t cooldown_min;
    uint32_t cooldown_start_ms;
    uint32_t cooldown_duration_ms;

    // Tasks and timers
    TaskHandle_t task;
    TimerHandle_t schedule_timer;
} state = {0};

static void set_state(sunrise_state_t new_state)
{
    if (state.state != new_state) {
        ESP_LOGI(TAG, "State: %d -> %d", state.state, new_state);
        state.state = new_state;
    }
}

static void update_leds(float progress)
{
    // Apply curve
    float curved_progress = sunrise_curve_apply(state.curve, progress);

    // Calculate ramped brightness
    uint8_t ramp_brightness = (uint8_t)(curved_progress * state.max_brightness);

    ESP_LOGD(TAG, "Sunrise: %.0f%% -> %d%%", progress * 100.0f, ramp_brightness);

    if (!state.animation_mode) {
        // Classic mode: interpolate color temperature from start to end
        uint16_t current_temp = sunrise_color_interpolate(
            progress, state.color_temp_start, state.color_temp_end);
        led_controller_set_color_temp(current_temp, ramp_brightness);
    } else {
        // Animated mode: wave with ramped base brightness
        // Note: base color is set once in sunrise_task() before the loop,
        // NOT here. Calling set_rgb() every frame triggers a uniform strip
        // refresh before set_pixel_brightnesses() applies the wave, causing flicker.
        float elapsed_sec = ((xTaskGetTickCount() * portTICK_PERIOD_MS) - state.start_time_ms) / 1000.0f;

        // Resolve waveform shape for this preset
        device_config_t *config = config_manager_get();
        wave_shape_t shape = {
            .kind = config->anim_waveform[state.animation_preset_id],
            .k = config->anim_wave_k_x10[state.animation_preset_id] / 10.0f,
            .i = config->anim_wave_i_x10[state.animation_preset_id] / 10.0f,
        };

        // Compute wave with ramped base and scaled amplitude
        wave_generator_compute_with_base(
            state.brightness_buffer,
            state.led_count,
            &state.preset_copy,
            elapsed_sec,
            ramp_brightness,    // Override base with ramping value
            curved_progress,    // Scale amplitude with ramp progress
            &shape
        );

        led_controller_set_pixel_brightnesses(state.brightness_buffer, state.led_count);
    }

    state.current_progress = progress;
}

static void sunrise_task(void *arg)
{
    ESP_LOGI(TAG, "Sunrise started: %lums, %dK->%dK, %d%%, animation=%d",
             state.duration_ms, state.color_temp_start, state.color_temp_end,
             state.max_brightness, state.animation_mode);

    state.start_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    set_state(SUNRISE_STATE_ACTIVE);

    // Set base color once before the loop (not per-frame) to avoid
    // double strip refresh causing flicker
    if (state.animation_mode) {
        uint8_t r, g, b;
        color_utils_kelvin_to_rgb(state.preset_copy.color_temp, &r, &g, &b);
        led_controller_set_rgb(r, g, b);
    }

    // Post alarm triggered event
    app_event_alarm_t event = {
        .alarm_id = state.scheduled_alarm_id >= 0 ? state.scheduled_alarm_id : 0,
        .progress_percent = 0,
    };
    esp_event_post(APP_EVENTS, APP_EVENT_ALARM_TRIGGERED, &event, sizeof(event), 0);

    // Use faster update interval for animation mode (smooth wave), slower for classic
    device_config_t *fps_config = config_manager_get();
    uint8_t anim_fps = fps_config->anim_fps >= 10 ? fps_config->anim_fps : 30;
    uint32_t update_interval_ms = state.animation_mode
        ? (1000 / anim_fps)
        : CONFIG_ML_SUNRISE_UPDATE_INTERVAL_MS;

    bool completion_posted = false;
    TickType_t last_wake = xTaskGetTickCount();

    while (state.state == SUNRISE_STATE_ACTIVE) {
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - state.start_time_ms;
        float progress = (float)elapsed / state.duration_ms;

        if (progress >= 1.0f) {
            progress = 1.0f;

            if (!state.animation_mode) {
                // Classic mode: stop at end
                update_leds(progress);
                if (!completion_posted) {
                    set_state(SUNRISE_STATE_COMPLETE);
                    event.progress_percent = 100;
                    esp_event_post(APP_EVENTS, APP_EVENT_ALARM_COMPLETE, &event, sizeof(event), 0);
                    completion_posted = true;
                }
                break;
            }

            // Animation mode: post completion once
            if (!completion_posted) {
                ESP_LOGI(TAG, "Ramp complete, continuing wave animation");
                event.progress_percent = 100;
                esp_event_post(APP_EVENTS, APP_EVENT_ALARM_COMPLETE, &event, sizeof(event), 0);
                completion_posted = true;
            }

            // If cooldown configured, break out to start cooldown phase
            if (state.cooldown_min > 0) {
                break;
            }
        }

        update_leds(progress);

        // Re-read in case the user changes the fps setting mid-animation
        if (state.animation_mode) {
            anim_fps = fps_config->anim_fps >= 10 ? fps_config->anim_fps : 30;
            update_interval_ms = 1000 / anim_fps;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(update_interval_ms));
    }

    // Cooldown phase: gradually dim to off
    if (state.state != SUNRISE_STATE_CANCELLED && state.cooldown_min > 0) {
        set_state(SUNRISE_STATE_COOLDOWN);
        state.cooldown_start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        state.cooldown_duration_ms = state.cooldown_min * 60 * 1000;

        ESP_LOGI(TAG, "Entering cooldown: %d min", state.cooldown_min);

        last_wake = xTaskGetTickCount();
        while (state.state == SUNRISE_STATE_COOLDOWN) {
            uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - state.cooldown_start_ms;
            float cooldown_progress = (float)elapsed / state.cooldown_duration_ms;

            if (cooldown_progress >= 1.0f) {
                led_controller_off();
                set_state(SUNRISE_STATE_IDLE);
                ESP_LOGI(TAG, "Cooldown complete, LEDs off");
                break;
            }

            // Reverse ramp: brightness goes max -> 0
            float reverse_progress = 1.0f - cooldown_progress;
            update_leds(reverse_progress);

            if (state.animation_mode) {
                anim_fps = fps_config->anim_fps >= 10 ? fps_config->anim_fps : 30;
                update_interval_ms = 1000 / anim_fps;
            }
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(update_interval_ms));
        }
    }

    if (state.state == SUNRISE_STATE_CANCELLED) {
        led_controller_off();
        esp_event_post(APP_EVENTS, APP_EVENT_ALARM_CANCELLED, &event, sizeof(event), 0);
    }

    ESP_LOGI(TAG, "Sunrise task ended");
    state.task = NULL;
    vTaskDelete(NULL);
}

static sunrise_curve_t alarm_curve_to_engine_curve(uint8_t val)
{
    switch (val) {
        case 0: return SUNRISE_CURVE_LOGARITHMIC;
        case 1: return SUNRISE_CURVE_INVERSE_LOG;
        case 2: return SUNRISE_CURVE_LINEAR;
        case 3: return SUNRISE_CURVE_SIGMOID;
        case 4: return SUNRISE_CURVE_EXPONENTIAL;
        default: return SUNRISE_CURVE_LOGARITHMIC;
    }
}

static void check_schedule(void)
{
    if (!time_manager_is_synced()) {
        state.scheduled_alarm_id = -1;
        state.minutes_until_alarm = -1;
        return;
    }

    int day = time_manager_get_day_of_week();
    int hour = time_manager_get_hour();
    int minute = time_manager_get_minute();

    if (day < 0 || hour < 0 || minute < 0) {
        return;
    }

    uint8_t alarm_id;
    alarm_config_t *next = config_manager_get_next_alarm(day, hour, minute, &alarm_id);

    if (next == NULL) {
        state.scheduled_alarm_id = -1;
        state.minutes_until_alarm = -1;
        set_state(SUNRISE_STATE_IDLE);
        return;
    }

    state.scheduled_alarm_id = alarm_id;
    state.minutes_until_alarm = time_manager_minutes_until(next->hour, next->minute, -1);

    ESP_LOGD(TAG, "Next alarm: #%d at %02d:%02d, in %d min",
             alarm_id, next->hour, next->minute, state.minutes_until_alarm);

    // Check if it's time to start (within 1 minute window)
    if (state.minutes_until_alarm <= 0 && state.state != SUNRISE_STATE_ACTIVE) {
        // Check dark mode before triggering
        if (config_manager_is_dark_mode_blocking(day, hour, minute)) {
            ESP_LOGI(TAG, "Dark mode blocking alarm #%d", alarm_id);
            return;
        }

        // Cancel any running cooldown before starting new alarm
        if (state.state == SUNRISE_STATE_COOLDOWN) {
            set_state(SUNRISE_STATE_CANCELLED);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Stop any running animation
        if (animation_engine_is_running()) {
            ESP_LOGI(TAG, "Stopping animation for scheduled sunrise");
            animation_engine_stop();
        }

        device_config_t *config = config_manager_get();

        // Setup animation mode if preset is valid
        if (next->animation_preset >= 0 && next->animation_preset < ANIMATION_MAX_PRESETS) {
            state.animation_mode = true;
            state.animation_preset_id = next->animation_preset;
            memcpy(&state.preset_copy, &config->animation_presets[next->animation_preset],
                   sizeof(animation_preset_t));
            state.led_count = led_controller_get_led_count();
            ESP_LOGI(TAG, "Starting animated sunrise #%d: preset=%d, %dmin, %d%%",
                     alarm_id, next->animation_preset, next->duration_min, next->brightness);
        } else {
            state.animation_mode = false;
            ESP_LOGI(TAG, "Starting sunrise #%d: %dmin, %dK, %d%%",
                     alarm_id, next->duration_min, next->color_temp, next->brightness);
        }

        state.duration_ms = next->duration_min * 60 * 1000;
        state.color_temp_end = next->color_temp;
        uint16_t ct_start = config->alarm_color_temp_start[alarm_id];
        state.color_temp_start = (ct_start > 0) ? ct_start : next->color_temp;
        state.max_brightness = next->brightness;
        state.cooldown_min = next->cooldown_min;
        state.curve = alarm_curve_to_engine_curve(config->alarm_curves[alarm_id]);

        // Limit to global max brightness
        if (state.max_brightness > config->brightness_max) {
            state.max_brightness = config->brightness_max;
        }

        // Start task
        if (state.task == NULL) {
            xTaskCreate(sunrise_task, "sunrise", 4096, NULL, 5, &state.task);
        }
    } else if (state.state == SUNRISE_STATE_IDLE || state.state == SUNRISE_STATE_COMPLETE) {
        set_state(SUNRISE_STATE_SCHEDULED);
    }
}

static void schedule_timer_callback(TimerHandle_t timer)
{
    // Dark mode enforcement: actively turn off lights
    if (time_manager_is_synced()) {
        int day = time_manager_get_day_of_week();
        int hour = time_manager_get_hour();
        int minute = time_manager_get_minute();

        if (day >= 0 && config_manager_is_dark_mode_blocking(day, hour, minute)) {
            if (state.state == SUNRISE_STATE_ACTIVE || state.state == SUNRISE_STATE_COOLDOWN) {
                ESP_LOGI(TAG, "Dark mode active, cancelling sunrise");
                set_state(SUNRISE_STATE_CANCELLED);
            }
            if (animation_engine_is_running()) {
                ESP_LOGI(TAG, "Dark mode active, stopping animation");
                animation_engine_stop();
            }
            return;
        }
    }

    if (state.state != SUNRISE_STATE_ACTIVE) {
        check_schedule();
    }
}

esp_err_t sunrise_engine_init(void)
{
    ESP_LOGI(TAG, "Initializing sunrise engine");

    if (state.initialized) {
        return ESP_OK;
    }

    state.state = SUNRISE_STATE_IDLE;
    state.curve = SUNRISE_CURVE_LOGARITHMIC;  // Most natural
    state.scheduled_alarm_id = -1;
    state.minutes_until_alarm = -1;
    state.animation_mode = false;
    state.animation_preset_id = -1;

    // Allocate brightness buffer for animated sunrise (max 300 LEDs)
    state.brightness_buffer = heap_caps_malloc(300, MALLOC_CAP_8BIT);
    if (state.brightness_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate brightness buffer");
        return ESP_ERR_NO_MEM;
    }

    // Create schedule check timer (every 30 seconds)
    state.schedule_timer = xTimerCreate("sunrise_sched", pdMS_TO_TICKS(30000),
                                        pdTRUE, NULL, schedule_timer_callback);
    if (state.schedule_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create timer");
        free(state.brightness_buffer);
        state.brightness_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    xTimerStart(state.schedule_timer, 0);

    state.initialized = true;
    ESP_LOGI(TAG, "Sunrise engine initialized");

    return ESP_OK;
}

void sunrise_engine_update_schedule(void)
{
    ESP_LOGI(TAG, "Updating schedule");
    check_schedule();
}

sunrise_state_t sunrise_engine_get_state(void)
{
    return state.state;
}

const char *sunrise_engine_get_state_str(void)
{
    switch (state.state) {
        case SUNRISE_STATE_IDLE:      return "idle";
        case SUNRISE_STATE_SCHEDULED: return "scheduled";
        case SUNRISE_STATE_ACTIVE:    return "active";
        case SUNRISE_STATE_COMPLETE:  return "complete";
        case SUNRISE_STATE_COOLDOWN:  return "cooldown";
        case SUNRISE_STATE_CANCELLED: return "cancelled";
        default:                      return "unknown";
    }
}

int sunrise_engine_get_progress(void)
{
    if (state.state == SUNRISE_STATE_ACTIVE) {
        return (int)(state.current_progress * 100);
    }
    if (state.state == SUNRISE_STATE_COOLDOWN) {
        return (int)(state.current_progress * 100);
    }
    return -1;
}

int sunrise_engine_get_active_alarm(void)
{
    return state.scheduled_alarm_id;
}

esp_err_t sunrise_engine_start_manual(uint8_t duration_min, uint16_t color_temp_start,
                                       uint16_t color_temp_end, uint8_t brightness,
                                       int8_t animation_preset, sunrise_curve_t curve)
{
    if (state.state == SUNRISE_STATE_ACTIVE) {
        ESP_LOGW(TAG, "Sunrise already active");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop any running animation
    if (animation_engine_is_running()) {
        ESP_LOGI(TAG, "Stopping animation for sunrise");
        animation_engine_stop();
    }

    device_config_t *config = config_manager_get();

    // Setup animation mode if preset is valid
    if (animation_preset >= 0 && animation_preset < ANIMATION_MAX_PRESETS) {
        state.animation_mode = true;
        state.animation_preset_id = animation_preset;
        memcpy(&state.preset_copy, &config->animation_presets[animation_preset],
               sizeof(animation_preset_t));
        state.led_count = led_controller_get_led_count();
        ESP_LOGI(TAG, "Starting manual animated sunrise: preset=%d, %d min, %d%%",
                 animation_preset, duration_min, brightness);
    } else {
        state.animation_mode = false;
        ESP_LOGI(TAG, "Starting manual sunrise: %d min, %dK->%dK, %d%%",
                 duration_min, color_temp_start, color_temp_end, brightness);
    }

    state.scheduled_alarm_id = -1;
    state.duration_ms = duration_min * 60 * 1000;
    state.color_temp_start = color_temp_start;
    state.color_temp_end = color_temp_end;
    state.max_brightness = brightness;
    state.curve = curve;
    state.cooldown_min = 0;  // No auto-off for manual tests

    // Limit to global max brightness
    if (state.max_brightness > config->brightness_max) {
        state.max_brightness = config->brightness_max;
    }

    if (state.task == NULL) {
        xTaskCreate(sunrise_task, "sunrise", 4096, NULL, 5, &state.task);
    }

    return ESP_OK;
}

void sunrise_engine_cancel(void)
{
    if (state.state == SUNRISE_STATE_ACTIVE || state.state == SUNRISE_STATE_COOLDOWN) {
        ESP_LOGI(TAG, "Cancelling sunrise");
        set_state(SUNRISE_STATE_CANCELLED);
    }
}

void sunrise_engine_stop(void)
{
    sunrise_engine_cancel();
    led_controller_off();

    if (state.state == SUNRISE_STATE_COMPLETE) {
        set_state(SUNRISE_STATE_IDLE);
    }
}

void sunrise_engine_set_curve(sunrise_curve_t curve)
{
    state.curve = curve;
    ESP_LOGI(TAG, "Curve set to: %d", curve);
}

void sunrise_engine_deinit(void)
{
    if (!state.initialized) return;

    sunrise_engine_cancel();

    if (state.schedule_timer) {
        xTimerStop(state.schedule_timer, 0);
        xTimerDelete(state.schedule_timer, 0);
        state.schedule_timer = NULL;
    }

    // Wait for task to exit
    if (state.task) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Free brightness buffer
    if (state.brightness_buffer) {
        free(state.brightness_buffer);
        state.brightness_buffer = NULL;
    }

    state.initialized = false;
}
