#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "status_led.h"

static const char *TAG = "status_led";

// Pattern timing definitions (in ms)
#define SLOW_BLINK_PERIOD       1000
#define FAST_BLINK_PERIOD       250
#define CONNECTED_FLASH_ON      50
#define CONNECTED_FLASH_PERIOD  5000
#define SUNRISE_PULSE_PERIOD    2000
#define MULTI_BLINK_ON          100
#define MULTI_BLINK_OFF         100
#define MULTI_BLINK_PAUSE       800

// State
static struct {
    gpio_num_t gpio;
    bool initialized;
    bool led_on;
    status_led_pattern_t pattern;
    TimerHandle_t timer;
    uint8_t blink_count;      // For multi-blink patterns
    uint8_t blink_target;     // Target blinks for pattern
    bool in_pause;            // In pause phase of multi-blink
} state = {0};

static void set_led(bool on)
{
    state.led_on = on;
    // Many ESP32 dev boards have active-low status LEDs
    gpio_set_level(state.gpio, on ? 1 : 0);
}

static void timer_callback(TimerHandle_t timer)
{
    switch (state.pattern) {
        case STATUS_LED_PATTERN_OFF:
            set_led(false);
            xTimerStop(timer, 0);
            break;

        case STATUS_LED_PATTERN_ON:
            set_led(true);
            xTimerStop(timer, 0);
            break;

        case STATUS_LED_PATTERN_SLOW_BLINK:
        case STATUS_LED_PATTERN_FAST_BLINK:
            set_led(!state.led_on);
            break;

        case STATUS_LED_PATTERN_DOUBLE_BLINK:
        case STATUS_LED_PATTERN_TRIPLE_BLINK:
            if (state.in_pause) {
                // End of pause, start new cycle
                state.in_pause = false;
                state.blink_count = 0;
                set_led(true);
                xTimerChangePeriod(timer, pdMS_TO_TICKS(MULTI_BLINK_ON), 0);
            } else if (state.led_on) {
                // LED was on, turn off
                set_led(false);
                state.blink_count++;
                if (state.blink_count >= state.blink_target) {
                    // Start pause
                    state.in_pause = true;
                    xTimerChangePeriod(timer, pdMS_TO_TICKS(MULTI_BLINK_PAUSE), 0);
                } else {
                    xTimerChangePeriod(timer, pdMS_TO_TICKS(MULTI_BLINK_OFF), 0);
                }
            } else {
                // LED was off, turn on for next blink
                set_led(true);
                xTimerChangePeriod(timer, pdMS_TO_TICKS(MULTI_BLINK_ON), 0);
            }
            break;

        case STATUS_LED_PATTERN_CONNECTED:
            if (state.led_on) {
                // Brief flash done
                set_led(false);
                xTimerChangePeriod(timer, pdMS_TO_TICKS(CONNECTED_FLASH_PERIOD - CONNECTED_FLASH_ON), 0);
            } else {
                // Time for flash
                set_led(true);
                xTimerChangePeriod(timer, pdMS_TO_TICKS(CONNECTED_FLASH_ON), 0);
            }
            break;

        case STATUS_LED_PATTERN_SUNRISE:
            // Simple toggle for pulse effect (could be PWM in future)
            set_led(!state.led_on);
            break;

        default:
            break;
    }
}

esp_err_t status_led_init(void)
{
    ESP_LOGI(TAG, "Initializing status LED on GPIO %d", CONFIG_ML_GPIO_STATUS_LED);

    state.gpio = CONFIG_ML_GPIO_STATUS_LED;

    // Configure GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << state.gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create timer
    state.timer = xTimerCreate("status_led", pdMS_TO_TICKS(1000), pdTRUE,
                               NULL, timer_callback);
    if (state.timer == NULL) {
        ESP_LOGE(TAG, "Failed to create timer");
        return ESP_ERR_NO_MEM;
    }

    state.initialized = true;
    state.pattern = STATUS_LED_PATTERN_OFF;
    set_led(false);

    ESP_LOGI(TAG, "Status LED initialized");
    return ESP_OK;
}

void status_led_set_pattern(status_led_pattern_t pattern)
{
    if (!state.initialized) return;

    ESP_LOGD(TAG, "Setting pattern: %d", pattern);
    state.pattern = pattern;
    state.blink_count = 0;
    state.in_pause = false;

    // Stop timer first
    xTimerStop(state.timer, 0);

    uint32_t period_ms = 0;

    switch (pattern) {
        case STATUS_LED_PATTERN_OFF:
            set_led(false);
            return;

        case STATUS_LED_PATTERN_ON:
            set_led(true);
            return;

        case STATUS_LED_PATTERN_SLOW_BLINK:
            period_ms = SLOW_BLINK_PERIOD / 2;
            set_led(true);
            break;

        case STATUS_LED_PATTERN_FAST_BLINK:
            period_ms = FAST_BLINK_PERIOD / 2;
            set_led(true);
            break;

        case STATUS_LED_PATTERN_DOUBLE_BLINK:
            state.blink_target = 2;
            period_ms = MULTI_BLINK_ON;
            set_led(true);
            break;

        case STATUS_LED_PATTERN_TRIPLE_BLINK:
            state.blink_target = 3;
            period_ms = MULTI_BLINK_ON;
            set_led(true);
            break;

        case STATUS_LED_PATTERN_CONNECTED:
            period_ms = CONNECTED_FLASH_ON;
            set_led(true);
            break;

        case STATUS_LED_PATTERN_SUNRISE:
            period_ms = SUNRISE_PULSE_PERIOD / 2;
            set_led(true);
            break;
    }

    if (period_ms > 0) {
        xTimerChangePeriod(state.timer, pdMS_TO_TICKS(period_ms), 0);
        xTimerStart(state.timer, 0);
    }
}

status_led_pattern_t status_led_get_pattern(void)
{
    return state.pattern;
}

void status_led_on(void)
{
    if (!state.initialized) return;
    set_led(true);
}

void status_led_off(void)
{
    if (!state.initialized) return;
    set_led(false);
}

void status_led_toggle(void)
{
    if (!state.initialized) return;
    set_led(!state.led_on);
}

void status_led_deinit(void)
{
    if (!state.initialized) return;

    if (state.timer) {
        xTimerStop(state.timer, 0);
        xTimerDelete(state.timer, 0);
        state.timer = NULL;
    }

    set_led(false);
    state.initialized = false;
}
