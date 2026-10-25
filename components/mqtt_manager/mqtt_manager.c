#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "cJSON.h"

#include "mqtt_manager.h"
#include "config_manager.h"
#include "led_controller.h"
#include "sunrise_engine.h"
#include "animation_engine.h"
#include "time_manager.h"

static const char *TAG = "mqtt_mgr";

// Forward declare app events
extern esp_event_base_t APP_EVENTS;
enum {
    _APP_EVENT_MQTT_CONNECTED = 17,
    _APP_EVENT_MQTT_DISCONNECTED,
};

// State
static esp_mqtt_client_handle_t client = NULL;
static bool connected = false;
static bool initialized = false;
static bool state_dirty = true;
static char device_id[5];  // Last 2 bytes of MAC as hex
static TimerHandle_t periodic_timer = NULL;

// Topic buffers
static char topic_buf[128];

static void get_device_id(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "%02X%02X", mac[4], mac[5]);
}

static int build_topic(const char *suffix)
{
    device_config_t *cfg = config_manager_get();
    return snprintf(topic_buf, sizeof(topic_buf), "%s/%s/%s",
                    cfg->mqtt.topic_prefix, device_id, suffix);
}

static void publish(const char *suffix, const char *payload, int qos, int retain)
{
    if (!connected || !client) return;
    build_topic(suffix);
    esp_mqtt_client_publish(client, topic_buf, payload, 0, qos, retain);
}

static void publish_json(const char *suffix, cJSON *json, int qos, int retain)
{
    char *str = cJSON_PrintUnformatted(json);
    if (str) {
        publish(suffix, str, qos, retain);
        free(str);
    }
}

// Build HA device block (shared by all entities)
static cJSON *build_device_block(void)
{
    device_config_t *cfg = config_manager_get();
    cJSON *dev = cJSON_CreateObject();

    char identifiers[32];
    snprintf(identifiers, sizeof(identifiers), "morninglight_%s", device_id);

    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(identifiers));
    cJSON_AddItemToObject(dev, "identifiers", ids);

    cJSON_AddStringToObject(dev, "name", cfg->mqtt.device_name);
    cJSON_AddStringToObject(dev, "manufacturer", "MorningLight");
    cJSON_AddStringToObject(dev, "model", "ESP32 Sunrise Alarm");
    cJSON_AddStringToObject(dev, "sw_version", "1.0");

    return dev;
}

static cJSON *build_origin(void)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", "MorningLight");
    cJSON_AddStringToObject(o, "sw", "1.0");
    cJSON_AddStringToObject(o, "url", "https://github.com/morninglight");
    return o;
}

static void build_availability(cJSON *json)
{
    build_topic("available");
    cJSON *avail = cJSON_CreateArray();
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "topic", topic_buf);
    cJSON_AddItemToArray(avail, a);
    cJSON_AddItemToObject(json, "availability", avail);
}

// Publish Home Assistant discovery configs
static void publish_ha_discovery(void)
{
    char disc_topic[128];
    char uid[32];
    char *payload;

    ESP_LOGI(TAG, "Publishing HA discovery configs");

    // --- Light entity ---
    {
        cJSON *json = cJSON_CreateObject();
        snprintf(uid, sizeof(uid), "morninglight_%s_light", device_id);
        cJSON_AddStringToObject(json, "unique_id", uid);
        cJSON_AddStringToObject(json, "name", "Light");
        cJSON_AddStringToObject(json, "schema", "json");

        build_topic("light/state");
        cJSON_AddStringToObject(json, "state_topic", topic_buf);
        build_topic("light/set");
        cJSON_AddStringToObject(json, "command_topic", topic_buf);

        cJSON_AddBoolToObject(json, "brightness", true);
        cJSON_AddNumberToObject(json, "brightness_scale", 100);
        cJSON_AddNumberToObject(json, "min_mireds", 154);
        cJSON_AddNumberToObject(json, "max_mireds", 500);
        cJSON *color_modes = cJSON_CreateArray();
        cJSON_AddItemToArray(color_modes, cJSON_CreateString("color_temp"));
        cJSON_AddItemToArray(color_modes, cJSON_CreateString("rgb"));
        cJSON_AddItemToObject(json, "supported_color_modes", color_modes);

        cJSON_AddItemToObject(json, "device", build_device_block());
        cJSON_AddItemToObject(json, "origin", build_origin());
        build_availability(json);

        payload = cJSON_PrintUnformatted(json);
        snprintf(disc_topic, sizeof(disc_topic), "homeassistant/light/morninglight_%s/light/config", device_id);
        esp_mqtt_client_publish(client, disc_topic, payload, 0, 1, 1);
        free(payload);
        cJSON_Delete(json);
    }

    // --- Sunrise progress sensor ---
    {
        cJSON *json = cJSON_CreateObject();
        snprintf(uid, sizeof(uid), "morninglight_%s_sunrise_pct", device_id);
        cJSON_AddStringToObject(json, "unique_id", uid);
        cJSON_AddStringToObject(json, "name", "Sunrise Progress");
        cJSON_AddStringToObject(json, "unit_of_measurement", "%");
        cJSON_AddStringToObject(json, "icon", "mdi:weather-sunset-up");
        cJSON_AddStringToObject(json, "value_template", "{{ value }}");

        build_topic("sensor/sunrise_progress");
        cJSON_AddStringToObject(json, "state_topic", topic_buf);

        cJSON_AddItemToObject(json, "device", build_device_block());
        cJSON_AddItemToObject(json, "origin", build_origin());
        build_availability(json);

        payload = cJSON_PrintUnformatted(json);
        snprintf(disc_topic, sizeof(disc_topic), "homeassistant/sensor/morninglight_%s/sunrise_progress/config", device_id);
        esp_mqtt_client_publish(client, disc_topic, payload, 0, 1, 1);
        free(payload);
        cJSON_Delete(json);
    }

    // --- Sunrise switch ---
    {
        cJSON *json = cJSON_CreateObject();
        snprintf(uid, sizeof(uid), "morninglight_%s_sunrise", device_id);
        cJSON_AddStringToObject(json, "unique_id", uid);
        cJSON_AddStringToObject(json, "name", "Sunrise");
        cJSON_AddStringToObject(json, "icon", "mdi:weather-sunset-up");

        build_topic("sunrise/state");
        cJSON_AddStringToObject(json, "state_topic", topic_buf);
        build_topic("sunrise/set");
        cJSON_AddStringToObject(json, "command_topic", topic_buf);

        cJSON_AddItemToObject(json, "device", build_device_block());
        cJSON_AddItemToObject(json, "origin", build_origin());
        build_availability(json);

        payload = cJSON_PrintUnformatted(json);
        snprintf(disc_topic, sizeof(disc_topic), "homeassistant/switch/morninglight_%s/sunrise/config", device_id);
        esp_mqtt_client_publish(client, disc_topic, payload, 0, 1, 1);
        free(payload);
        cJSON_Delete(json);
    }

    // --- Animation select ---
    {
        cJSON *json = cJSON_CreateObject();
        snprintf(uid, sizeof(uid), "morninglight_%s_animation", device_id);
        cJSON_AddStringToObject(json, "unique_id", uid);
        cJSON_AddStringToObject(json, "name", "Animation");
        cJSON_AddStringToObject(json, "icon", "mdi:sine-wave");

        build_topic("animation/state");
        cJSON_AddStringToObject(json, "state_topic", topic_buf);
        build_topic("animation/set");
        cJSON_AddStringToObject(json, "command_topic", topic_buf);

        cJSON *options = cJSON_CreateArray();
        cJSON_AddItemToArray(options, cJSON_CreateString("Off"));
        for (int i = 0; i < ANIMATION_MAX_PRESETS; i++) {
            animation_preset_t *p = animation_engine_get_preset(i);
            if (p) {
                cJSON_AddItemToArray(options, cJSON_CreateString(p->name));
            }
        }
        cJSON_AddItemToObject(json, "options", options);

        cJSON_AddItemToObject(json, "device", build_device_block());
        cJSON_AddItemToObject(json, "origin", build_origin());
        build_availability(json);

        payload = cJSON_PrintUnformatted(json);
        snprintf(disc_topic, sizeof(disc_topic), "homeassistant/select/morninglight_%s/animation/config", device_id);
        esp_mqtt_client_publish(client, disc_topic, payload, 0, 1, 1);
        free(payload);
        cJSON_Delete(json);
    }
}

// Publish light state
static void publish_light_state(void)
{
    uint8_t brightness = led_controller_get_brightness();
    uint8_t r, g, b;
    led_controller_get_rgb(&r, &g, &b);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "state", brightness > 0 ? "ON" : "OFF");
    cJSON_AddNumberToObject(json, "brightness", brightness);

    cJSON *color = cJSON_AddObjectToObject(json, "color");
    cJSON_AddNumberToObject(color, "r", r);
    cJSON_AddNumberToObject(color, "g", g);
    cJSON_AddNumberToObject(color, "b", b);

    publish_json("light/state", json, 0, 1);
    cJSON_Delete(json);
}

// Publish sunrise state
static void publish_sunrise_state(void)
{
    sunrise_state_t state = sunrise_engine_get_state();
    bool active = (state == SUNRISE_STATE_ACTIVE || state == SUNRISE_STATE_COMPLETE || state == SUNRISE_STATE_COOLDOWN);
    publish("sunrise/state", active ? "ON" : "OFF", 0, 1);

    int progress = sunrise_engine_get_progress();
    if (progress < 0) progress = 0;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d", progress);
    publish("sensor/sunrise_progress", buf, 0, 0);
}

// Publish animation state
static void publish_animation_state(void)
{
    if (animation_engine_is_running()) {
        int8_t preset_id = animation_engine_get_active_preset();
        if (preset_id >= 0 && preset_id < ANIMATION_MAX_PRESETS) {
            animation_preset_t *p = animation_engine_get_preset(preset_id);
            if (p) {
                publish("animation/state", p->name, 0, 1);
                return;
            }
        }
    }
    publish("animation/state", "Off", 0, 1);
}

// Publish all state
static void publish_all_state(void)
{
    publish_light_state();
    publish_sunrise_state();
    publish_animation_state();
    state_dirty = false;
}

// Handle light commands
static void handle_light_command(const char *data, int data_len)
{
    char *buf = malloc(data_len + 1);
    if (!buf) return;
    memcpy(buf, data, data_len);
    buf[data_len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) return;

    cJSON *state = cJSON_GetObjectItem(json, "state");

    if (cJSON_IsString(state) && strcmp(state->valuestring, "OFF") == 0) {
        // OFF: cancel sunrise, stop animation, turn off
        sunrise_engine_cancel();
        if (animation_engine_is_running()) {
            animation_engine_stop();
        }
        led_controller_off();
    } else if (cJSON_IsString(state) && strcmp(state->valuestring, "ON") == 0) {
        // Check dark mode
        if (time_manager_is_synced()) {
            int d = time_manager_get_day_of_week();
            int h = time_manager_get_hour();
            int m = time_manager_get_minute();
            if (d >= 0 && config_manager_is_dark_mode_blocking(d, h, m)) {
                cJSON_Delete(json);
                return;
            }
        }

        // Stop animation if running
        if (animation_engine_is_running()) {
            animation_engine_stop();
        }

        cJSON *color_temp = cJSON_GetObjectItem(json, "color_temp");
        cJSON *color = cJSON_GetObjectItem(json, "color");
        cJSON *brightness = cJSON_GetObjectItem(json, "brightness");

        uint8_t bright = led_controller_get_brightness();
        if (cJSON_IsNumber(brightness)) {
            bright = (uint8_t)brightness->valuedouble;
        }
        if (bright == 0) bright = 100;  // Default to full if turning on from off

        if (cJSON_IsNumber(color_temp)) {
            // Mireds to Kelvin
            int mireds = (int)color_temp->valuedouble;
            if (mireds < 154) mireds = 154;
            if (mireds > 500) mireds = 500;
            uint16_t kelvin = 1000000 / mireds;
            led_controller_set_color_temp(kelvin, bright);
        } else if (cJSON_IsObject(color)) {
            cJSON *cr = cJSON_GetObjectItem(color, "r");
            cJSON *cg = cJSON_GetObjectItem(color, "g");
            cJSON *cb = cJSON_GetObjectItem(color, "b");
            if (cJSON_IsNumber(cr) && cJSON_IsNumber(cg) && cJSON_IsNumber(cb)) {
                led_controller_set_rgb((uint8_t)cr->valuedouble,
                                       (uint8_t)cg->valuedouble,
                                       (uint8_t)cb->valuedouble);
                led_controller_set_brightness(bright);
            }
        } else if (cJSON_IsNumber(brightness)) {
            // Just brightness change
            led_controller_set_brightness(bright);
        } else {
            // ON with no parameters, set default warm white
            led_controller_set_color_temp(3000, 100);
        }
    }

    cJSON_Delete(json);
    state_dirty = true;

    // Publish updated state immediately
    if (connected) {
        publish_light_state();
    }
}

// Handle sunrise commands
static void handle_sunrise_command(const char *data, int data_len)
{
    if (data_len >= 2 && strncmp(data, "ON", 2) == 0) {
        sunrise_engine_start_manual(30, 2000, 3000, 100, -1, SUNRISE_CURVE_LOGARITHMIC);
    } else if (data_len >= 3 && strncmp(data, "OFF", 3) == 0) {
        sunrise_engine_cancel();
    }
    state_dirty = true;
    if (connected) {
        publish_sunrise_state();
    }
}

// Handle animation commands
static void handle_animation_command(const char *data, int data_len)
{
    char buf[16];
    int len = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = '\0';

    if (strcmp(buf, "Off") == 0) {
        animation_engine_stop();
    } else {
        // Find preset by name
        for (int i = 0; i < ANIMATION_MAX_PRESETS; i++) {
            animation_preset_t *p = animation_engine_get_preset(i);
            if (p && strcmp(buf, p->name) == 0) {
                animation_engine_start(i);
                break;
            }
        }
    }
    state_dirty = true;
    if (connected) {
        publish_animation_state();
    }
}

// Subscribe to command topics
static void subscribe_topics(void)
{
    build_topic("light/set");
    esp_mqtt_client_subscribe(client, topic_buf, 1);

    build_topic("sunrise/set");
    esp_mqtt_client_subscribe(client, topic_buf, 1);

    build_topic("animation/set");
    esp_mqtt_client_subscribe(client, topic_buf, 1);

    // Subscribe to HA status for re-discovery on HA restart
    esp_mqtt_client_subscribe(client, "homeassistant/status", 1);
}

// MQTT event handler
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected to broker");
            connected = true;
            esp_event_post(APP_EVENTS, _APP_EVENT_MQTT_CONNECTED, NULL, 0, 0);

            // Publish online status
            publish("available", "online", 1, 1);

            // Subscribe and publish discovery
            subscribe_topics();
            publish_ha_discovery();
            publish_all_state();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            connected = false;
            esp_event_post(APP_EVENTS, _APP_EVENT_MQTT_DISCONNECTED, NULL, 0, 0);
            break;

        case MQTT_EVENT_DATA: {
            // Route to appropriate handler based on topic
            if (!event->topic || !event->data) break;

            char topic_suffix[64] = {0};
            // Extract suffix after device_id/
            device_config_t *cfg = config_manager_get();
            char prefix[64];
            snprintf(prefix, sizeof(prefix), "%s/%s/", cfg->mqtt.topic_prefix, device_id);
            size_t prefix_len = strlen(prefix);

            if (event->topic_len > (int)prefix_len &&
                strncmp(event->topic, prefix, prefix_len) == 0) {
                int suffix_len = event->topic_len - prefix_len;
                if (suffix_len < (int)sizeof(topic_suffix)) {
                    memcpy(topic_suffix, event->topic + prefix_len, suffix_len);
                    topic_suffix[suffix_len] = '\0';
                }
            }

            if (strcmp(topic_suffix, "light/set") == 0) {
                handle_light_command(event->data, event->data_len);
            } else if (strcmp(topic_suffix, "sunrise/set") == 0) {
                handle_sunrise_command(event->data, event->data_len);
            } else if (strcmp(topic_suffix, "animation/set") == 0) {
                handle_animation_command(event->data, event->data_len);
            } else if (event->topic_len == 20 &&
                       strncmp(event->topic, "homeassistant/status", 20) == 0) {
                // HA restart: re-publish discovery
                if (event->data_len >= 6 && strncmp(event->data, "online", 6) == 0) {
                    ESP_LOGI(TAG, "HA came online, re-publishing discovery");
                    publish_ha_discovery();
                    publish_all_state();
                }
            }
            break;
        }

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error event");
            break;

        default:
            break;
    }
}

// Periodic timer callback (runs every 30s)
static void periodic_timer_cb(TimerHandle_t timer)
{
    if (!connected) return;

    // Publish state if dirty
    if (state_dirty) {
        publish_light_state();
        publish_sunrise_state();
        publish_animation_state();
        state_dirty = false;
    }
}

esp_err_t mqtt_manager_init(void)
{
    if (initialized) return ESP_OK;

    ESP_LOGI(TAG, "Initializing MQTT manager");
    get_device_id();
    ESP_LOGI(TAG, "Device ID: %s", device_id);

    initialized = true;
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (!initialized) return ESP_ERR_INVALID_STATE;

    device_config_t *cfg = config_manager_get();
    if (!cfg->mqtt.enabled || cfg->mqtt.broker_uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT not enabled or no broker configured");
        // If client is running, stop it
        if (client) {
            mqtt_manager_stop();
        }
        return ESP_OK;
    }

    // Stop existing client if any
    if (client) {
        mqtt_manager_stop();
    }

    ESP_LOGI(TAG, "Starting MQTT client, broker: %s", cfg->mqtt.broker_uri);

    // Build LWT topic
    build_topic("available");
    char lwt_topic[128];
    strncpy(lwt_topic, topic_buf, sizeof(lwt_topic) - 1);
    lwt_topic[sizeof(lwt_topic) - 1] = '\0';

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = cfg->mqtt.broker_uri,
        .credentials.username = cfg->mqtt.username[0] ? cfg->mqtt.username : NULL,
        .credentials.authentication.password = cfg->mqtt.password[0] ? cfg->mqtt.password : NULL,
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .buffer.size = 2048,
        .buffer.out_size = 2048,
        .task.stack_size = 6144,
        .network.reconnect_timeout_ms = 10000,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(client);
        client = NULL;
        return err;
    }

    // Create periodic timer if not exists
    if (!periodic_timer) {
        periodic_timer = xTimerCreate("mqtt_periodic", pdMS_TO_TICKS(30000),
                                       pdTRUE, NULL, periodic_timer_cb);
        if (periodic_timer) {
            xTimerStart(periodic_timer, 0);
        }
    }

    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (client) {
        ESP_LOGI(TAG, "Stopping MQTT client");
        // Publish offline before disconnect
        if (connected) {
            publish("available", "offline", 1, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
        connected = false;
    }

    if (periodic_timer) {
        xTimerStop(periodic_timer, 0);
        xTimerDelete(periodic_timer, 0);
        periodic_timer = NULL;
    }

    return ESP_OK;
}

bool mqtt_manager_is_connected(void)
{
    return connected;
}

void mqtt_manager_publish_state(void)
{
    if (connected) {
        publish_all_state();
    }
}

void mqtt_manager_notify_light_changed(void)
{
    state_dirty = true;
    if (connected) {
        publish_light_state();
    }
}

void mqtt_manager_deinit(void)
{
    mqtt_manager_stop();
    initialized = false;
}

void mqtt_manager_republish_discovery(void)
{
    if (connected) {
        publish_ha_discovery();
    }
}
