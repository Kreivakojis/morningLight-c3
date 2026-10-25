#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "app_events.h"
#include "led_controller.h"
#include "status_led.h"
#include "wifi_manager.h"
#include "dns_server.h"
#include "web_server.h"
#include "config_manager.h"
#include "time_manager.h"
#include "sunrise_engine.h"
#include "animation_engine.h"
#include "button_handler.h"
#include "mqtt_manager.h"

static const char *TAG = "app_main";

// Define the application event base
ESP_EVENT_DEFINE_BASE(APP_EVENTS);

static void app_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == APP_EVENTS) {
        switch (event_id) {
            case APP_EVENT_WIFI_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected");
                status_led_set_pattern(STATUS_LED_PATTERN_CONNECTED);
                break;

            case APP_EVENT_WIFI_DISCONNECTED:
                ESP_LOGI(TAG, "WiFi disconnected");
                status_led_set_pattern(STATUS_LED_PATTERN_SLOW_BLINK);
                break;

            case APP_EVENT_WIFI_AP_STARTED:
                ESP_LOGI(TAG, "AP mode started");
                status_led_set_pattern(STATUS_LED_PATTERN_FAST_BLINK);
                break;

            case APP_EVENT_WIFI_GOT_IP:
                ESP_LOGI(TAG, "Got IP address");
                time_manager_start_sync();
                mqtt_manager_start();
                break;

            case APP_EVENT_TIME_SYNCED:
                ESP_LOGI(TAG, "Time synchronized");
                sunrise_engine_update_schedule();
                break;

            case APP_EVENT_ALARM_TRIGGERED: {
                app_event_alarm_t *alarm = (app_event_alarm_t *)event_data;
                ESP_LOGI(TAG, "Alarm %d triggered", alarm->alarm_id);
                break;
            }

            case APP_EVENT_BUTTON_FACTORY_RESET:
                ESP_LOGW(TAG, "Factory reset requested");
                config_manager_factory_reset();
                esp_restart();
                break;

            // Scenario button: diagnostic hooks for testing LED output with
            // WiFi's RMT-interrupting interrupt load removed from the
            // picture, without needing the web UI. Short press toggles the
            // "Gentle" animation preset; long press permanently kills WiFi
            // for the rest of this boot (reboot to restore it).
            case APP_EVENT_BUTTON_SHORT_PRESS: {
                app_event_button_t *btn = (app_event_button_t *)event_data;
                if (btn->button_id == BUTTON_SCENARIO) {
                    if (animation_engine_is_running()) {
                        ESP_LOGI(TAG, "Scenario button: stopping animation");
                        animation_engine_stop();
                    } else {
                        ESP_LOGI(TAG, "Scenario button: starting 'Gentle' animation (preset 0)");
                        animation_engine_start(0);
                    }
                }
                break;
            }

            case APP_EVENT_BUTTON_LONG_PRESS: {
                app_event_button_t *btn = (app_event_button_t *)event_data;
                if (btn->button_id == BUTTON_SCENARIO) {
                    ESP_LOGW(TAG, "Scenario button long-press: killing WiFi for the rest of this boot");
                    wifi_manager_deinit();
                }
                break;
            }

            case APP_EVENT_CONFIG_CHANGED:
                ESP_LOGI(TAG, "Configuration changed");
                sunrise_engine_update_schedule();
                break;

            case APP_EVENT_LED_TEST_START: {
                app_event_led_test_t *led = (app_event_led_test_t *)event_data;
                led_controller_set_rgb(led->r, led->g, led->b);
                led_controller_set_brightness(led->brightness);
                break;
            }

            case APP_EVENT_LED_TEST_STOP:
                led_controller_set_brightness(0);
                break;

            default:
                break;
        }
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    ESP_LOGI(TAG, "MorningLight-Mini starting...");

    // Initialize NVS
    ESP_ERROR_CHECK(init_nvs());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register application event handler
    ESP_ERROR_CHECK(esp_event_handler_register(APP_EVENTS, ESP_EVENT_ANY_ID,
                                                app_event_handler, NULL));

    // Initialize components in order
    ESP_ERROR_CHECK(status_led_init());
    status_led_set_pattern(STATUS_LED_PATTERN_FAST_BLINK);

    // Config manager must be initialized before LED controller
    ESP_ERROR_CHECK(config_manager_init());

    ESP_ERROR_CHECK(led_controller_init());

    ESP_ERROR_CHECK(button_handler_init());

    ESP_ERROR_CHECK(time_manager_init());

    ESP_ERROR_CHECK(sunrise_engine_init());

    ESP_ERROR_CHECK(animation_engine_init());

    // Initialize WiFi - this will start AP or STA mode based on config
    ESP_ERROR_CHECK(wifi_manager_init());

    // Initialize MQTT (auto-starts when WiFi connects)
    ESP_ERROR_CHECK(mqtt_manager_init());

    // Start web server
    ESP_ERROR_CHECK(web_server_start());

    // Post system ready event
    esp_event_post(APP_EVENTS, APP_EVENT_SYSTEM_READY, NULL, 0, portMAX_DELAY);

    ESP_LOGI(TAG, "MorningLight-Mini initialized successfully");
}
