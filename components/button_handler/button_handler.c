#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "button_handler.h"

static const char *TAG = "button";

// Forward declare app events
extern esp_event_base_t APP_EVENTS;
enum {
    APP_EVENT_BUTTON_SHORT_PRESS = 10,
    APP_EVENT_BUTTON_LONG_PRESS,
    APP_EVENT_BUTTON_FACTORY_RESET,
};

typedef struct {
    uint8_t button_id;
    uint32_t duration_ms;
} app_event_button_t;

// Timing constants
#define DEBOUNCE_MS          50
#define SHORT_PRESS_MAX_MS   1000
#define LONG_PRESS_MS        1000
#define FACTORY_RESET_MS     5000
#define POLL_INTERVAL_MS     20

// Button configuration
typedef struct {
    gpio_num_t gpio;
    bool active_low;
    bool pressed;
    uint32_t press_start_ms;
    uint32_t last_change_ms;
    bool long_press_sent;
    bool factory_reset_sent;
} button_state_t;

static struct {
    bool initialized;
    button_state_t buttons[BUTTON_MAX];
    TimerHandle_t poll_timer;
} state = {0};

static uint32_t get_time_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool read_button(button_id_t id)
{
    bool level = gpio_get_level(state.buttons[id].gpio);
    return state.buttons[id].active_low ? !level : level;
}

static void poll_buttons(TimerHandle_t timer)
{
    uint32_t now = get_time_ms();

    for (int i = 0; i < BUTTON_MAX; i++) {
        button_state_t *btn = &state.buttons[i];
        bool current = read_button(i);

        // Debounce
        if ((now - btn->last_change_ms) < DEBOUNCE_MS) {
            continue;
        }

        if (current && !btn->pressed) {
            // Button pressed
            btn->pressed = true;
            btn->press_start_ms = now;
            btn->last_change_ms = now;
            btn->long_press_sent = false;
            btn->factory_reset_sent = false;
            ESP_LOGD(TAG, "Button %d pressed", i);

        } else if (!current && btn->pressed) {
            // Button released
            btn->pressed = false;
            btn->last_change_ms = now;
            uint32_t duration = now - btn->press_start_ms;

            ESP_LOGD(TAG, "Button %d released after %lu ms", i, duration);

            // Only send short press if we haven't sent long press
            if (!btn->long_press_sent && duration < SHORT_PRESS_MAX_MS) {
                app_event_button_t event = { .button_id = i, .duration_ms = duration };
                esp_event_post(APP_EVENTS, APP_EVENT_BUTTON_SHORT_PRESS, &event, sizeof(event), 0);
                ESP_LOGI(TAG, "Button %d: short press (%lu ms)", i, duration);
            }

        } else if (btn->pressed) {
            // Button held
            uint32_t duration = now - btn->press_start_ms;

            // Check for factory reset (5+ seconds on reset button)
            if (i == BUTTON_RESET && duration >= FACTORY_RESET_MS && !btn->factory_reset_sent) {
                btn->factory_reset_sent = true;
                app_event_button_t event = { .button_id = i, .duration_ms = duration };
                esp_event_post(APP_EVENTS, APP_EVENT_BUTTON_FACTORY_RESET, &event, sizeof(event), 0);
                ESP_LOGW(TAG, "Button %d: factory reset triggered", i);
            }
            // Check for long press
            else if (duration >= LONG_PRESS_MS && !btn->long_press_sent) {
                btn->long_press_sent = true;
                app_event_button_t event = { .button_id = i, .duration_ms = duration };
                esp_event_post(APP_EVENTS, APP_EVENT_BUTTON_LONG_PRESS, &event, sizeof(event), 0);
                ESP_LOGI(TAG, "Button %d: long press (%lu ms)", i, duration);
            }
        }
    }
}

esp_err_t button_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing button handler");

    if (state.initialized) {
        return ESP_OK;
    }

    memset(&state, 0, sizeof(state));

    // Configure reset button (GPIO 0, active low)
    state.buttons[BUTTON_RESET].gpio = CONFIG_ML_GPIO_BTN_RESET;
    state.buttons[BUTTON_RESET].active_low = true;

    // Configure scenario button
    state.buttons[BUTTON_SCENARIO].gpio = CONFIG_ML_GPIO_BTN_SCENARIO;
    state.buttons[BUTTON_SCENARIO].active_low = true;

    // Configure GPIOs
    for (int i = 0; i < BUTTON_MAX; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << state.buttons[i].gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = state.buttons[i].active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = state.buttons[i].active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
    }

    // Create polling timer
    state.poll_timer = xTimerCreate("btn_poll", pdMS_TO_TICKS(POLL_INTERVAL_MS),
                                    pdTRUE, NULL, poll_buttons);
    if (state.poll_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create timer");
        return ESP_ERR_NO_MEM;
    }

    xTimerStart(state.poll_timer, 0);

    state.initialized = true;
    ESP_LOGI(TAG, "Button handler initialized: reset=GPIO%d, scenario=GPIO%d",
             CONFIG_ML_GPIO_BTN_RESET, CONFIG_ML_GPIO_BTN_SCENARIO);

    return ESP_OK;
}

bool button_handler_is_pressed(button_id_t button)
{
    if (button >= BUTTON_MAX) return false;
    return state.buttons[button].pressed;
}

uint32_t button_handler_press_duration(button_id_t button)
{
    if (button >= BUTTON_MAX || !state.buttons[button].pressed) {
        return 0;
    }
    return get_time_ms() - state.buttons[button].press_start_ms;
}

void button_handler_deinit(void)
{
    if (!state.initialized) return;

    if (state.poll_timer) {
        xTimerStop(state.poll_timer, 0);
        xTimerDelete(state.poll_timer, 0);
        state.poll_timer = NULL;
    }

    state.initialized = false;
}
