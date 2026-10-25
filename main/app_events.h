#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// Declare the application event base
ESP_EVENT_DECLARE_BASE(APP_EVENTS);

// Application event IDs
typedef enum {
    // WiFi events
    APP_EVENT_WIFI_CONNECTED,
    APP_EVENT_WIFI_DISCONNECTED,
    APP_EVENT_WIFI_AP_STARTED,
    APP_EVENT_WIFI_AP_STOPPED,
    APP_EVENT_WIFI_GOT_IP,

    // Time events
    APP_EVENT_TIME_SYNCED,
    APP_EVENT_TIME_SYNC_FAILED,

    // Alarm events
    APP_EVENT_ALARM_TRIGGERED,
    APP_EVENT_ALARM_COMPLETE,
    APP_EVENT_ALARM_CANCELLED,

    // Button events
    APP_EVENT_BUTTON_SHORT_PRESS,
    APP_EVENT_BUTTON_LONG_PRESS,
    APP_EVENT_BUTTON_FACTORY_RESET,

    // Config events
    APP_EVENT_CONFIG_CHANGED,
    APP_EVENT_CONFIG_SAVED,

    // LED events
    APP_EVENT_LED_TEST_START,
    APP_EVENT_LED_TEST_STOP,

    // MQTT events
    APP_EVENT_MQTT_CONNECTED,
    APP_EVENT_MQTT_DISCONNECTED,

    // System events
    APP_EVENT_SYSTEM_READY,
    APP_EVENT_SYSTEM_ERROR,
} app_event_id_t;

// Event data structures
typedef struct {
    uint32_t ip_addr;
} app_event_ip_t;

typedef struct {
    uint8_t button_id;
    uint32_t duration_ms;
} app_event_button_t;

typedef struct {
    uint8_t alarm_id;
    uint32_t progress_percent;
} app_event_alarm_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;
} app_event_led_test_t;

#ifdef __cplusplus
}
#endif

#endif // APP_EVENTS_H
