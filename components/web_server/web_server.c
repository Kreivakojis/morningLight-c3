#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "cJSON.h"

#include "esp_system.h"
#include "web_server.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "time_manager.h"
#include "led_controller.h"
#include "sunrise_engine.h"
#include "animation_engine.h"
#include "dns_server.h"
#include "mqtt_manager.h"

static const char *TAG = "web_server";

// Embedded files
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char style_css_start[] asm("_binary_style_css_start");
extern const char style_css_end[] asm("_binary_style_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");

// Forward declare app events
extern esp_event_base_t APP_EVENTS;
enum {
    APP_EVENT_LED_TEST_START = 14,
    APP_EVENT_LED_TEST_STOP,
};

typedef struct {
    uint8_t r, g, b, brightness;
} app_event_led_test_t;

static httpd_handle_t server = NULL;

// Helper to send JSON response
static esp_err_t send_json_response(httpd_req_t *req, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, str);
    free(str);
    cJSON_Delete(json);
    return ESP_OK;
}

// Helper to send JSON error response (instead of HTML from httpd_resp_send_err)
static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"success\":false,\"error\":\"%s\"}", message);
    httpd_resp_sendstr(req, buf);
    return ESP_FAIL;
}

// Helper to parse JSON from request
static cJSON *parse_json_body(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 4096) {
        return NULL;
    }

    char *buf = malloc(content_len + 1);
    if (buf == NULL) {
        return NULL;
    }

    int received = httpd_req_recv(req, buf, content_len);
    if (received != content_len) {
        free(buf);
        return NULL;
    }
    buf[content_len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

// GET / - Serve index.html
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
    return ESP_OK;
}

// GET /style.css
static esp_err_t style_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, style_css_start, style_css_end - style_css_start);
    return ESP_OK;
}

// GET /app.js
static esp_err_t app_js_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, app_js_start, app_js_end - app_js_start);
    return ESP_OK;
}

// GET /api/status
static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();

    // WiFi status
    cJSON_AddStringToObject(json, "wifi_state",
        wifi_manager_is_connected() ? "connected" : "disconnected");

    char ap_ssid[33];
    wifi_manager_get_ap_ssid(ap_ssid);
    cJSON_AddStringToObject(json, "ap_ssid", ap_ssid);

    uint32_t ip = wifi_manager_get_ip();
    if (ip) {
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR,
                 (int)(ip & 0xFF), (int)((ip >> 8) & 0xFF),
                 (int)((ip >> 16) & 0xFF), (int)((ip >> 24) & 0xFF));
        cJSON_AddStringToObject(json, "ip", ip_str);
    }

    // Time
    cJSON_AddBoolToObject(json, "time_synced", time_manager_is_synced());
    if (time_manager_is_synced()) {
        char time_str[32];
        time_manager_get_time_str(time_str, sizeof(time_str));
        cJSON_AddStringToObject(json, "time", time_str);
    }

    // Sunrise engine
    cJSON_AddStringToObject(json, "sunrise_state",
        sunrise_engine_get_state_str());
    cJSON_AddNumberToObject(json, "brightness", led_controller_get_brightness());

    // Dark mode status
    bool dm_active = false;
    if (time_manager_is_synced()) {
        int d = time_manager_get_day_of_week();
        int h = time_manager_get_hour();
        int m = time_manager_get_minute();
        if (d >= 0) dm_active = config_manager_is_dark_mode_blocking(d, h, m);
    }
    cJSON_AddBoolToObject(json, "dark_mode_active", dm_active);

    // MQTT status
    cJSON_AddBoolToObject(json, "mqtt_connected", mqtt_manager_is_connected());

    // Config status
    device_config_t *config = config_manager_get();
    cJSON_AddBoolToObject(json, "setup_complete", config ? config->setup_complete : false);

    return send_json_response(req, json);
}

// GET /api/wifi/scan
static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    // Start scan if not complete
    if (!wifi_manager_scan_complete()) {
        wifi_manager_start_scan();
        // Wait for scan (max 5 seconds)
        for (int i = 0; i < 50 && !wifi_manager_scan_complete(); i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    cJSON *json = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(json, "networks");

    wifi_scan_result_t results[20];
    uint16_t count = wifi_manager_get_scan_results(results, 20);

    for (uint16_t i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(net, "secure", results[i].authmode != 0);
        cJSON_AddItemToArray(networks, net);
    }

    return send_json_response(req, json);
}

// POST /api/wifi/connect
static esp_err_t api_wifi_connect_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(json, "password");

    if (!cJSON_IsString(ssid_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }

    const char *ssid = ssid_item->valuestring;
    const char *password = cJSON_IsString(password_item) ? password_item->valuestring : NULL;

    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    // Save credentials
    device_config_t *config = config_manager_get();
    strncpy(config->wifi.ssid, ssid, sizeof(config->wifi.ssid) - 1);
    if (password) {
        strncpy(config->wifi.password, password, sizeof(config->wifi.password) - 1);
    } else {
        config->wifi.password[0] = '\0';
    }
    config->setup_complete = true;
    config_manager_save();

    // Connect
    esp_err_t ret = wifi_manager_connect(ssid, password);

    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", ret == ESP_OK);
    return send_json_response(req, resp);
}

// GET /api/config
static esp_err_t api_config_get_handler(httpd_req_t *req)
{
    device_config_t *config = config_manager_get();
    cJSON *json = cJSON_CreateObject();

    cJSON_AddStringToObject(json, "timezone", config->timezone);
    cJSON_AddNumberToObject(json, "brightness_max", config->brightness_max);
    cJSON_AddBoolToObject(json, "setup_complete", config->setup_complete);

    // LED configuration: single addressable strip
    cJSON_AddBoolToObject(json, "ws1_enabled", config->ws1_enabled);
    cJSON_AddNumberToObject(json, "ws1_led_count", config->ws1_led_count);
    cJSON_AddNumberToObject(json, "anim_fps", config->anim_fps);
    cJSON_AddNumberToObject(json, "gamma", config->gamma_x10 / 10.0);

    // WS2811 animated-pixel-path temporal dithering (optional, experimental)
    cJSON_AddBoolToObject(json, "dither_enabled", config->dither_enabled);
    cJSON_AddNumberToObject(json, "dither_freq_hz", config->dither_freq_hz);
    cJSON_AddNumberToObject(json, "dither_max_brightness_pct", config->dither_max_brightness_pct);

    // WiFi (don't expose password)
    cJSON *wifi = cJSON_AddObjectToObject(json, "wifi");
    cJSON_AddStringToObject(wifi, "ssid", config->wifi.ssid);

    return send_json_response(req, json);
}

// POST /api/config
static esp_err_t api_config_post_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    device_config_t *config = config_manager_get();
    bool restart_required = false;

    cJSON *tz = cJSON_GetObjectItem(json, "timezone");
    if (cJSON_IsString(tz)) {
        strncpy(config->timezone, tz->valuestring, sizeof(config->timezone) - 1);
        time_manager_set_timezone(config->timezone);
    }

    cJSON *brightness = cJSON_GetObjectItem(json, "brightness_max");
    if (cJSON_IsNumber(brightness)) {
        config->brightness_max = (uint8_t)brightness->valuedouble;
    }

    // Output enable/count changes require a restart (drivers/GPIOs are
    // configured at led_controller_init() time)
    cJSON *ws1_en = cJSON_GetObjectItem(json, "ws1_enabled");
    if (cJSON_IsBool(ws1_en)) {
        bool v = cJSON_IsTrue(ws1_en);
        if (v != config->ws1_enabled) restart_required = true;
        config->ws1_enabled = v;
    }
    cJSON *ws1_cnt = cJSON_GetObjectItem(json, "ws1_led_count");
    if (cJSON_IsNumber(ws1_cnt)) {
        uint16_t new_count = (uint16_t)ws1_cnt->valuedouble;
        if (new_count >= 1 && new_count <= 300) {
            if (new_count != config->ws1_led_count) restart_required = true;
            config->ws1_led_count = new_count;
        }
    }
    // Animation frame rate (applied live by the animation/sunrise tasks, no restart needed)
    cJSON *anim_fps = cJSON_GetObjectItem(json, "anim_fps");
    if (cJSON_IsNumber(anim_fps)) {
        int v = (int)anim_fps->valuedouble;
        config->anim_fps = (v >= 45) ? 60 : 30;
    }

    // WS2811 temporal dithering settings (applied live, no restart needed)
    cJSON *dither = cJSON_GetObjectItem(json, "dither_enabled");
    if (cJSON_IsBool(dither)) {
        config->dither_enabled = cJSON_IsTrue(dither);
    }
    cJSON *dither_freq = cJSON_GetObjectItem(json, "dither_freq_hz");
    if (cJSON_IsNumber(dither_freq)) {
        int v = (int)dither_freq->valuedouble;
        if (v < 1) v = 1;
        if (v > 60) v = 60;
        config->dither_freq_hz = (uint8_t)v;
    }
    cJSON *dither_max = cJSON_GetObjectItem(json, "dither_max_brightness_pct");
    if (cJSON_IsNumber(dither_max)) {
        int v = (int)dither_max->valuedouble;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        config->dither_max_brightness_pct = (uint8_t)v;
    }

    cJSON *gamma = cJSON_GetObjectItem(json, "gamma");
    if (cJSON_IsNumber(gamma)) {
        int g = (int)(gamma->valuedouble * 10 + 0.5);
        if (g < 10) g = 10;
        if (g > 40) g = 40;
        config->gamma_x10 = (uint8_t)g;
    }

    ESP_LOGI(TAG, "Saving config: ws1=%d(%d)",
             config->ws1_enabled, config->ws1_led_count);

    config_manager_save();
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddBoolToObject(resp, "restart_required", restart_required);
    return send_json_response(req, resp);
}

// GET /api/alarms
static esp_err_t api_alarms_get_handler(httpd_req_t *req)
{
    device_config_t *config = config_manager_get();
    cJSON *json = cJSON_CreateObject();
    cJSON *alarms = cJSON_AddArrayToObject(json, "alarms");

    for (int i = 0; i < 8; i++) {
        alarm_config_t *alarm = &config->alarms[i];
        if (!alarm->enabled && alarm->hour == 0 && alarm->minute == 0) {
            continue;
        }

        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "id", i);
        cJSON_AddBoolToObject(a, "enabled", alarm->enabled);
        cJSON_AddNumberToObject(a, "hour", alarm->hour);
        cJSON_AddNumberToObject(a, "minute", alarm->minute);
        cJSON_AddNumberToObject(a, "duration_min", alarm->duration_min);
        cJSON_AddNumberToObject(a, "days_mask", alarm->days_mask);
        cJSON_AddNumberToObject(a, "color_temp", alarm->color_temp);
        cJSON_AddNumberToObject(a, "brightness", alarm->brightness);
        cJSON_AddNumberToObject(a, "animation_preset", alarm->animation_preset);
        cJSON_AddNumberToObject(a, "cooldown_min", alarm->cooldown_min);
        cJSON_AddNumberToObject(a, "brightness_curve", config->alarm_curves[i]);
        cJSON_AddNumberToObject(a, "color_temp_start", config->alarm_color_temp_start[i]);
        cJSON_AddStringToObject(a, "name", config->alarm_names[i]);
        cJSON_AddItemToArray(alarms, a);
    }

    return send_json_response(req, json);
}

// POST /api/alarms
static esp_err_t api_alarms_post_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    device_config_t *config = config_manager_get();

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    int id = cJSON_IsNumber(id_item) ? (int)id_item->valuedouble : -1;

    // Find slot if no ID specified
    if (id < 0 || id >= 8) {
        for (int i = 0; i < 8; i++) {
            if (!config->alarms[i].enabled) {
                id = i;
                break;
            }
        }
    }

    if (id < 0 || id >= 8) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No alarm slots available");
        return ESP_FAIL;
    }

    alarm_config_t *alarm = &config->alarms[id];

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(enabled)) alarm->enabled = cJSON_IsTrue(enabled);

    cJSON *hour = cJSON_GetObjectItem(json, "hour");
    if (cJSON_IsNumber(hour)) alarm->hour = (uint8_t)hour->valuedouble;

    cJSON *minute = cJSON_GetObjectItem(json, "minute");
    if (cJSON_IsNumber(minute)) alarm->minute = (uint8_t)minute->valuedouble;

    cJSON *duration = cJSON_GetObjectItem(json, "duration_min");
    if (cJSON_IsNumber(duration)) alarm->duration_min = (uint8_t)duration->valuedouble;

    cJSON *days = cJSON_GetObjectItem(json, "days_mask");
    if (cJSON_IsNumber(days)) alarm->days_mask = (uint8_t)days->valuedouble;

    cJSON *color_temp = cJSON_GetObjectItem(json, "color_temp");
    if (cJSON_IsNumber(color_temp)) alarm->color_temp = (uint16_t)color_temp->valuedouble;

    cJSON *brightness = cJSON_GetObjectItem(json, "brightness");
    if (cJSON_IsNumber(brightness)) alarm->brightness = (uint8_t)brightness->valuedouble;

    cJSON *anim_preset = cJSON_GetObjectItem(json, "animation_preset");
    if (cJSON_IsNumber(anim_preset)) {
        int8_t val = (int8_t)anim_preset->valuedouble;
        if (val >= -1 && val < ANIMATION_MAX_PRESETS) {
            alarm->animation_preset = val;
        }
    }

    cJSON *cooldown = cJSON_GetObjectItem(json, "cooldown_min");
    if (cJSON_IsNumber(cooldown)) {
        uint8_t val = (uint8_t)cooldown->valuedouble;
        if (val <= 60) alarm->cooldown_min = val;
    }

    cJSON *curve = cJSON_GetObjectItem(json, "brightness_curve");
    if (cJSON_IsNumber(curve)) {
        uint8_t val = (uint8_t)curve->valuedouble;
        if (val <= 4) config->alarm_curves[id] = val;
    }

    cJSON *ct_start = cJSON_GetObjectItem(json, "color_temp_start");
    if (cJSON_IsNumber(ct_start)) {
        config->alarm_color_temp_start[id] = (uint16_t)ct_start->valuedouble;
    }

    cJSON *name = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name)) {
        strncpy(config->alarm_names[id], name->valuestring, 15);
        config->alarm_names[id][15] = '\0';
    }

    config_manager_save();
    sunrise_engine_update_schedule();

    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "id", id);
    return send_json_response(req, resp);
}

// POST /api/led/test
static esp_err_t api_led_test_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Stop any running animation before LED test
    if (animation_engine_is_running()) {
        animation_engine_stop();
    }

    cJSON *off = cJSON_GetObjectItem(json, "off");
    cJSON *color_temp = cJSON_GetObjectItem(json, "color_temp");
    cJSON *brightness = cJSON_GetObjectItem(json, "brightness");

    // Check dark mode (skip for "off" commands)
    if (!cJSON_IsTrue(off) && time_manager_is_synced()) {
        int d = time_manager_get_day_of_week();
        int h = time_manager_get_hour();
        int m = time_manager_get_minute();
        if (d >= 0 && config_manager_is_dark_mode_blocking(d, h, m)) {
            cJSON_Delete(json);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", false);
            cJSON_AddStringToObject(resp, "error", "Dark mode active");
            return send_json_response(req, resp);
        }
    }

    if (cJSON_IsTrue(off)) {
        // Cancel active sunrise (alarm without cooldown scenario)
        sunrise_engine_cancel();
        led_controller_off();
    } else if (cJSON_IsNumber(color_temp) && cJSON_IsNumber(brightness)) {
        uint16_t temp = (uint16_t)color_temp->valuedouble;
        uint8_t bright = (uint8_t)brightness->valuedouble;
        led_controller_set_color_temp(temp, bright);
    }

    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    return send_json_response(req, resp);
}

// POST /api/time/sync
static esp_err_t api_time_sync_handler(httpd_req_t *req)
{
    time_manager_force_sync();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    return send_json_response(req, resp);
}

// GET /api/animation/presets
static esp_err_t api_animation_presets_get_handler(httpd_req_t *req)
{
    device_config_t *config = config_manager_get();
    cJSON *json = cJSON_CreateObject();
    cJSON *presets = cJSON_AddArrayToObject(json, "presets");

    for (int i = 0; i < ANIMATION_MAX_PRESETS; i++) {
        animation_preset_t *p = &config->animation_presets[i];
        cJSON *preset = cJSON_CreateObject();
        cJSON_AddNumberToObject(preset, "id", i);
        cJSON_AddStringToObject(preset, "name", p->name);
        cJSON_AddNumberToObject(preset, "wavelength", p->wavelength);
        cJSON_AddNumberToObject(preset, "amplitude", p->amplitude);
        cJSON_AddNumberToObject(preset, "speed", p->speed);
        cJSON_AddNumberToObject(preset, "base_brightness", p->base_brightness);
        cJSON_AddNumberToObject(preset, "variation", p->variation);
        cJSON_AddNumberToObject(preset, "color_temp", p->color_temp);
        cJSON_AddNumberToObject(preset, "waveform", config->anim_waveform[i]);
        cJSON_AddNumberToObject(preset, "wave_k", config->anim_wave_k_x10[i] / 10.0);
        cJSON_AddNumberToObject(preset, "wave_i", config->anim_wave_i_x10[i] / 10.0);
        cJSON_AddItemToArray(presets, preset);
    }

    cJSON_AddNumberToObject(json, "active", animation_engine_get_active_preset());
    cJSON_AddBoolToObject(json, "running", animation_engine_is_running());

    return send_json_response(req, json);
}

// POST /api/animation/presets
static esp_err_t api_animation_presets_post_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (!cJSON_IsNumber(id_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing preset id");
        return ESP_FAIL;
    }

    int id = (int)id_item->valuedouble;
    if (id < 0 || id >= ANIMATION_MAX_PRESETS) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid preset id");
        return ESP_FAIL;
    }

    animation_preset_t *preset = animation_engine_get_preset(id);
    if (!preset) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get preset");
        return ESP_FAIL;
    }

    device_config_t *config = config_manager_get();

    // Update preset fields
    cJSON *name = cJSON_GetObjectItem(json, "name");
    if (cJSON_IsString(name)) {
        strncpy(preset->name, name->valuestring, sizeof(preset->name) - 1);
        preset->name[sizeof(preset->name) - 1] = '\0';
    }

    cJSON *wavelength = cJSON_GetObjectItem(json, "wavelength");
    if (cJSON_IsNumber(wavelength)) {
        preset->wavelength = (float)wavelength->valuedouble;
        if (preset->wavelength < 2.0f) preset->wavelength = 2.0f;
        if (preset->wavelength > 300.0f) preset->wavelength = 300.0f;
    }

    cJSON *amplitude = cJSON_GetObjectItem(json, "amplitude");
    if (cJSON_IsNumber(amplitude)) {
        preset->amplitude = (uint8_t)amplitude->valuedouble;
        if (preset->amplitude > 100) preset->amplitude = 100;
    }

    cJSON *speed = cJSON_GetObjectItem(json, "speed");
    if (cJSON_IsNumber(speed)) {
        preset->speed = (float)speed->valuedouble;
        if (preset->speed < 0.0f) preset->speed = 0.0f;
        if (preset->speed > 5.0f) preset->speed = 5.0f;
    }

    cJSON *base = cJSON_GetObjectItem(json, "base_brightness");
    if (cJSON_IsNumber(base)) {
        preset->base_brightness = (uint8_t)base->valuedouble;
        if (preset->base_brightness > 100) preset->base_brightness = 100;
    }

    cJSON *variation = cJSON_GetObjectItem(json, "variation");
    if (cJSON_IsNumber(variation)) {
        preset->variation = (uint8_t)variation->valuedouble;
        if (preset->variation > 100) preset->variation = 100;
    }

    cJSON *color_temp = cJSON_GetObjectItem(json, "color_temp");
    if (cJSON_IsNumber(color_temp)) {
        preset->color_temp = (uint16_t)color_temp->valuedouble;
        if (preset->color_temp < 2000) preset->color_temp = 2000;
        if (preset->color_temp > 6500) preset->color_temp = 6500;
    }

    cJSON *waveform = cJSON_GetObjectItem(json, "waveform");
    if (cJSON_IsNumber(waveform)) {
        int v = (int)waveform->valuedouble;
        config->anim_waveform[id] = (v == WAVE_SHAPE_HYPERBOLIC_AC || v == WAVE_SHAPE_HYPERBOLIC_PULSE) ? (uint8_t)v : WAVE_SHAPE_SINE;
    }

    cJSON *wave_k = cJSON_GetObjectItem(json, "wave_k");
    if (cJSON_IsNumber(wave_k)) {
        int v = (int)(wave_k->valuedouble * 10.0 + 0.5);
        if (v < 5) v = 5;
        if (v > 200) v = 200;
        config->anim_wave_k_x10[id] = (uint8_t)v;
    }

    cJSON *wave_i = cJSON_GetObjectItem(json, "wave_i");
    if (cJSON_IsNumber(wave_i)) {
        int v = (int)(wave_i->valuedouble * 10.0 + 0.5);
        if (v < 2) v = 2;
        if (v > 40) v = 40;
        config->anim_wave_i_x10[id] = (uint8_t)v;
    }

    config_manager_save();
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "id", id);
    return send_json_response(req, resp);
}

// POST /api/animation/start
static esp_err_t api_animation_start_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *preset_id = cJSON_GetObjectItem(json, "preset_id");
    if (!cJSON_IsNumber(preset_id)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing preset_id");
        return ESP_FAIL;
    }

    int id = (int)preset_id->valuedouble;
    cJSON_Delete(json);

    // Check dark mode before starting animation
    if (time_manager_is_synced()) {
        int d = time_manager_get_day_of_week();
        int h = time_manager_get_hour();
        int m = time_manager_get_minute();
        if (d >= 0 && config_manager_is_dark_mode_blocking(d, h, m)) {
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "success", false);
            cJSON_AddStringToObject(resp, "error", "Dark mode active");
            return send_json_response(req, resp);
        }
    }

    esp_err_t ret = animation_engine_start(id);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", ret == ESP_OK);
    if (ret != ESP_OK) {
        cJSON_AddStringToObject(resp, "error", esp_err_to_name(ret));
    }
    return send_json_response(req, resp);
}

// POST /api/animation/stop
static esp_err_t api_animation_stop_handler(httpd_req_t *req)
{
    animation_engine_stop();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    return send_json_response(req, resp);
}

// GET /api/darkmode
static esp_err_t api_darkmode_get_handler(httpd_req_t *req)
{
    device_config_t *config = config_manager_get();
    cJSON *json = cJSON_CreateObject();
    cJSON *schedules = cJSON_AddArrayToObject(json, "schedules");

    for (int i = 0; i < DARK_MODE_MAX_SCHEDULES; i++) {
        dark_mode_schedule_t *s = &config->dark_schedules[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", i);
        cJSON_AddBoolToObject(item, "enabled", s->enabled);
        cJSON_AddNumberToObject(item, "start_hour", s->start_hour);
        cJSON_AddNumberToObject(item, "start_minute", s->start_minute);
        cJSON_AddNumberToObject(item, "end_hour", s->end_hour);
        cJSON_AddNumberToObject(item, "end_minute", s->end_minute);
        cJSON_AddNumberToObject(item, "days_mask", s->days_mask);
        cJSON_AddBoolToObject(item, "allow_override", s->allow_override);
        cJSON_AddItemToArray(schedules, item);
    }

    return send_json_response(req, json);
}

// POST /api/darkmode
static esp_err_t api_darkmode_post_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    device_config_t *config = config_manager_get();

    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (!cJSON_IsNumber(id_item)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
        return ESP_FAIL;
    }

    int id = (int)id_item->valuedouble;
    if (id < 0 || id >= DARK_MODE_MAX_SCHEDULES) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid id (0-2)");
        return ESP_FAIL;
    }

    dark_mode_schedule_t *s = &config->dark_schedules[id];

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(enabled)) s->enabled = cJSON_IsTrue(enabled);

    cJSON *start_hour = cJSON_GetObjectItem(json, "start_hour");
    if (cJSON_IsNumber(start_hour)) {
        uint8_t v = (uint8_t)start_hour->valuedouble;
        if (v <= 23) s->start_hour = v;
    }

    cJSON *start_minute = cJSON_GetObjectItem(json, "start_minute");
    if (cJSON_IsNumber(start_minute)) {
        uint8_t v = (uint8_t)start_minute->valuedouble;
        if (v <= 59) s->start_minute = v;
    }

    cJSON *end_hour = cJSON_GetObjectItem(json, "end_hour");
    if (cJSON_IsNumber(end_hour)) {
        uint8_t v = (uint8_t)end_hour->valuedouble;
        if (v <= 23) s->end_hour = v;
    }

    cJSON *end_minute = cJSON_GetObjectItem(json, "end_minute");
    if (cJSON_IsNumber(end_minute)) {
        uint8_t v = (uint8_t)end_minute->valuedouble;
        if (v <= 59) s->end_minute = v;
    }

    cJSON *days_mask = cJSON_GetObjectItem(json, "days_mask");
    if (cJSON_IsNumber(days_mask)) s->days_mask = (uint8_t)days_mask->valuedouble;

    cJSON *allow_override = cJSON_GetObjectItem(json, "allow_override");
    if (cJSON_IsBool(allow_override)) s->allow_override = cJSON_IsTrue(allow_override);

    config_manager_save();
    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddNumberToObject(resp, "id", id);
    return send_json_response(req, resp);
}

// 404 handler for captive portal redirect
static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    // Check for captive portal detection URLs
    const char *uri = req->uri;
    if (strstr(uri, "generate_204") || strstr(uri, "hotspot-detect") ||
        strstr(uri, "connecttest") || strstr(uri, "ncsi")) {
        // Redirect to root
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    // Default: redirect to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// OPTIONS handler for CORS
static esp_err_t options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// GET /api/config/export
static const char *config_export_header =
    "// MorningLight-Mini Configuration Export\n"
    "//\n"
    "// Fields:\n"
    "//   timezone         - POSIX timezone string (e.g. \"UTC0\", \"UTC-2\")\n"
    "//   brightness_max   - Global brightness cap, 10-100 (%)\n"
    "//   ws1_enabled      - Addressable strip populated, true/false\n"
    "//   ws1_led_count    - Number of LEDs on the strip, 1-300\n"
    "//   anim_fps         - Animation/wave frame rate, 30 or 60\n"
    "//   gamma            - Gamma correction value, 1.0-4.0 (default 2.2)\n"
    "//   dither_enabled       - Temporal dithering on animated WS2811 pixel path, true/false (experimental)\n"
    "//   dither_freq_hz       - Dither update frequency in Hz, 1-60 (default 10)\n"
    "//   dither_max_brightness_pct - Dithering only below this brightness %, 0-100 (default 50)\n"
    "//\n"
    "//   alarms (array of 8 slots):\n"
    "//     enabled          - true/false\n"
    "//     hour             - 0-23\n"
    "//     minute           - 0-59\n"
    "//     duration_min     - Sunrise duration, 5-120 minutes\n"
    "//     days_mask        - Bitmask: bit0=Sun, bit1=Mon, ... bit6=Sat (e.g. 62 = Mon-Fri)\n"
    "//     color_temp       - Color temperature, 2000-6500 Kelvin\n"
    "//     brightness       - Target brightness, 0-100 (%)\n"
    "//     animation_preset - -1 = classic, 0-4 = animation preset index\n"
    "//     cooldown_min     - Auto-off delay, 0 = disabled, 1-60 minutes\n"
    "//     brightness_curve - 0=logarithmic, 1=inverse_log, 2=linear, 3=sigmoid, 4=exponential\n"
    "//     color_temp_start - Starting color temp (K), 0 = same as color_temp (no ramp)\n"
    "//     name             - Optional display name, max 15 chars\n"
    "//\n"
    "//   animation_presets (array of 5 slots):\n"
    "//     name             - Display name, max 11 chars\n"
    "//     wavelength       - 2.0-300.0 LEDs between peaks\n"
    "//     amplitude        - 0-100 (%) contrast\n"
    "//     speed            - 0.0-5.0 cycles/sec\n"
    "//     base_brightness  - 0-100 (%) midpoint\n"
    "//     variation        - 0-100 (%) organic noise\n"
    "//     color_temp       - 2000-6500 Kelvin\n"
    "//     waveform         - 0=Sine, 1=Hyperbolic AC, 2=Hyperbolic Pulse (rectified, no negative lobe)\n"
    "//     wave_k           - Hyperbolic sharpness (AC/Pulse), 0.5-20.0 (default 10.0)\n"
    "//     wave_i           - Hyperbolic crest shape (AC/Pulse), 0.2-4.0 (default 2.0 = unity peak)\n"
    "//\n"
    "//   dark_mode (array of 3 slots):\n"
    "//     enabled          - true/false\n"
    "//     start_hour       - 0-23\n"
    "//     start_minute     - 0-59\n"
    "//     end_hour         - 0-23\n"
    "//     end_minute       - 0-59\n"
    "//     days_mask        - Bitmask (same as alarms)\n"
    "//     allow_override   - true = lights can still operate during this window\n"
    "//\n"
    "// WiFi credentials are excluded for security.\n"
    "// To import: upload this file via the web UI.\n"
    "//\n";

static esp_err_t api_config_export_handler(httpd_req_t *req)
{
    device_config_t *config = config_manager_get();
    cJSON *json = cJSON_CreateObject();

    // General settings
    cJSON_AddStringToObject(json, "timezone", config->timezone);
    cJSON_AddNumberToObject(json, "brightness_max", config->brightness_max);
    cJSON_AddBoolToObject(json, "ws1_enabled", config->ws1_enabled);
    cJSON_AddNumberToObject(json, "ws1_led_count", config->ws1_led_count);
    cJSON_AddNumberToObject(json, "anim_fps", config->anim_fps);
    cJSON_AddNumberToObject(json, "gamma", config->gamma_x10 / 10.0);
    cJSON_AddBoolToObject(json, "dither_enabled", config->dither_enabled);
    cJSON_AddNumberToObject(json, "dither_freq_hz", config->dither_freq_hz);
    cJSON_AddNumberToObject(json, "dither_max_brightness_pct", config->dither_max_brightness_pct);

    // Alarms (all 8 slots)
    cJSON *alarms = cJSON_AddArrayToObject(json, "alarms");
    for (int i = 0; i < 8; i++) {
        alarm_config_t *a = &config->alarms[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddBoolToObject(item, "enabled", a->enabled);
        cJSON_AddNumberToObject(item, "hour", a->hour);
        cJSON_AddNumberToObject(item, "minute", a->minute);
        cJSON_AddNumberToObject(item, "duration_min", a->duration_min);
        cJSON_AddNumberToObject(item, "days_mask", a->days_mask);
        cJSON_AddNumberToObject(item, "color_temp", a->color_temp);
        cJSON_AddNumberToObject(item, "brightness", a->brightness);
        cJSON_AddNumberToObject(item, "animation_preset", a->animation_preset);
        cJSON_AddNumberToObject(item, "cooldown_min", a->cooldown_min);
        cJSON_AddNumberToObject(item, "brightness_curve", config->alarm_curves[i]);
        cJSON_AddNumberToObject(item, "color_temp_start", config->alarm_color_temp_start[i]);
        cJSON_AddStringToObject(item, "name", config->alarm_names[i]);
        cJSON_AddItemToArray(alarms, item);
    }

    // Animation presets (all 5 slots)
    cJSON *presets = cJSON_AddArrayToObject(json, "animation_presets");
    for (int i = 0; i < ANIMATION_MAX_PRESETS; i++) {
        animation_preset_t *p = &config->animation_presets[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", p->name);
        cJSON_AddNumberToObject(item, "wavelength", p->wavelength);
        cJSON_AddNumberToObject(item, "amplitude", p->amplitude);
        cJSON_AddNumberToObject(item, "speed", p->speed);
        cJSON_AddNumberToObject(item, "base_brightness", p->base_brightness);
        cJSON_AddNumberToObject(item, "variation", p->variation);
        cJSON_AddNumberToObject(item, "color_temp", p->color_temp);
        cJSON_AddNumberToObject(item, "waveform", config->anim_waveform[i]);
        cJSON_AddNumberToObject(item, "wave_k", config->anim_wave_k_x10[i] / 10.0);
        cJSON_AddNumberToObject(item, "wave_i", config->anim_wave_i_x10[i] / 10.0);
        cJSON_AddItemToArray(presets, item);
    }

    // Dark mode (all 3 slots)
    cJSON *dark = cJSON_AddArrayToObject(json, "dark_mode");
    for (int i = 0; i < DARK_MODE_MAX_SCHEDULES; i++) {
        dark_mode_schedule_t *s = &config->dark_schedules[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddBoolToObject(item, "enabled", s->enabled);
        cJSON_AddNumberToObject(item, "start_hour", s->start_hour);
        cJSON_AddNumberToObject(item, "start_minute", s->start_minute);
        cJSON_AddNumberToObject(item, "end_hour", s->end_hour);
        cJSON_AddNumberToObject(item, "end_minute", s->end_minute);
        cJSON_AddNumberToObject(item, "days_mask", s->days_mask);
        cJSON_AddBoolToObject(item, "allow_override", s->allow_override);
        cJSON_AddItemToArray(dark, item);
    }

    // MQTT settings (exclude password for security)
    cJSON *mqtt = cJSON_AddObjectToObject(json, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", config->mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "broker_uri", config->mqtt.broker_uri);
    cJSON_AddStringToObject(mqtt, "username", config->mqtt.username);
    cJSON_AddStringToObject(mqtt, "topic_prefix", config->mqtt.topic_prefix);
    cJSON_AddStringToObject(mqtt, "device_name", config->mqtt.device_name);

    char *printed = cJSON_Print(json);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"morninglight-config.json\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, config_export_header);
    httpd_resp_sendstr_chunk(req, printed);
    httpd_resp_sendstr_chunk(req, NULL);

    free(printed);
    return ESP_OK;
}

// POST /api/config/import
static esp_err_t api_config_import_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) {
        return send_json_error(req, HTTPD_400, "Body too large or empty");
    }

    char *buf = malloc(content_len + 1);
    if (buf == NULL) {
        return send_json_error(req, HTTPD_500, "Out of memory");
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            free(buf);
            return send_json_error(req, HTTPD_400, "Incomplete body");
        }
        received += ret;
    }
    buf[content_len] = '\0';

    // Skip comment header — find first '{'
    char *json_start = strchr(buf, '{');
    if (json_start == NULL) {
        free(buf);
        return send_json_error(req, HTTPD_400, "No JSON object found");
    }

    cJSON *json = cJSON_Parse(json_start);
    free(buf);
    if (json == NULL) {
        return send_json_error(req, HTTPD_400, "Invalid JSON");
    }

    device_config_t *config = config_manager_get();
    bool restart_required = false;

    // General settings
    cJSON *tz = cJSON_GetObjectItem(json, "timezone");
    if (cJSON_IsString(tz)) {
        strncpy(config->timezone, tz->valuestring, sizeof(config->timezone) - 1);
        config->timezone[sizeof(config->timezone) - 1] = '\0';
        time_manager_set_timezone(config->timezone);
    }

    cJSON *brightness = cJSON_GetObjectItem(json, "brightness_max");
    if (cJSON_IsNumber(brightness)) {
        config->brightness_max = (uint8_t)brightness->valuedouble;
    }

    cJSON *ws1_en_i = cJSON_GetObjectItem(json, "ws1_enabled");
    if (cJSON_IsBool(ws1_en_i)) {
        bool v = cJSON_IsTrue(ws1_en_i);
        if (v != config->ws1_enabled) restart_required = true;
        config->ws1_enabled = v;
    }
    cJSON *ws1_cnt_i = cJSON_GetObjectItem(json, "ws1_led_count");
    if (cJSON_IsNumber(ws1_cnt_i)) {
        uint16_t new_count = (uint16_t)ws1_cnt_i->valuedouble;
        if (new_count >= 1 && new_count <= 300) {
            if (new_count != config->ws1_led_count) restart_required = true;
            config->ws1_led_count = new_count;
        }
    }
    cJSON *anim_fps_i = cJSON_GetObjectItem(json, "anim_fps");
    if (cJSON_IsNumber(anim_fps_i)) {
        int v = (int)anim_fps_i->valuedouble;
        config->anim_fps = (v >= 45) ? 60 : 30;
    }

    cJSON *dither_i = cJSON_GetObjectItem(json, "dither_enabled");
    if (cJSON_IsBool(dither_i)) {
        config->dither_enabled = cJSON_IsTrue(dither_i);
    }
    cJSON *dither_freq_i = cJSON_GetObjectItem(json, "dither_freq_hz");
    if (cJSON_IsNumber(dither_freq_i)) {
        int v = (int)dither_freq_i->valuedouble;
        if (v < 1) v = 1;
        if (v > 60) v = 60;
        config->dither_freq_hz = (uint8_t)v;
    }
    cJSON *dither_max_i = cJSON_GetObjectItem(json, "dither_max_brightness_pct");
    if (cJSON_IsNumber(dither_max_i)) {
        int v = (int)dither_max_i->valuedouble;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        config->dither_max_brightness_pct = (uint8_t)v;
    }

    cJSON *gamma_item = cJSON_GetObjectItem(json, "gamma");
    if (cJSON_IsNumber(gamma_item)) {
        int g = (int)(gamma_item->valuedouble * 10 + 0.5);
        if (g >= 10 && g <= 40) {
            config->gamma_x10 = (uint8_t)g;
        }
    }

    // Alarms — if present, replace all 8 slots
    cJSON *alarms_arr = cJSON_GetObjectItem(json, "alarms");
    if (cJSON_IsArray(alarms_arr)) {
        memset(config->alarms, 0, sizeof(config->alarms));
        memset(config->alarm_curves, 0, sizeof(config->alarm_curves));
        memset(config->alarm_color_temp_start, 0, sizeof(config->alarm_color_temp_start));
        memset(config->alarm_names, 0, sizeof(config->alarm_names));
        int count = cJSON_GetArraySize(alarms_arr);
        if (count > 8) count = 8;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(alarms_arr, i);
            alarm_config_t *a = &config->alarms[i];

            cJSON *v;
            v = cJSON_GetObjectItem(item, "enabled");
            if (cJSON_IsBool(v)) a->enabled = cJSON_IsTrue(v);

            v = cJSON_GetObjectItem(item, "hour");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 23) a->hour = val; }

            v = cJSON_GetObjectItem(item, "minute");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 59) a->minute = val; }

            v = cJSON_GetObjectItem(item, "duration_min");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val >= 5 && val <= 120) a->duration_min = val; }

            v = cJSON_GetObjectItem(item, "days_mask");
            if (cJSON_IsNumber(v)) a->days_mask = (uint8_t)v->valuedouble;

            v = cJSON_GetObjectItem(item, "color_temp");
            if (cJSON_IsNumber(v)) { uint16_t val = (uint16_t)v->valuedouble; if (val >= 2000 && val <= 6500) a->color_temp = val; }

            v = cJSON_GetObjectItem(item, "brightness");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 100) a->brightness = val; }

            v = cJSON_GetObjectItem(item, "animation_preset");
            if (cJSON_IsNumber(v)) { int8_t val = (int8_t)v->valuedouble; if (val >= -1 && val < ANIMATION_MAX_PRESETS) a->animation_preset = val; }

            v = cJSON_GetObjectItem(item, "cooldown_min");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 60) a->cooldown_min = val; }

            v = cJSON_GetObjectItem(item, "brightness_curve");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 4) config->alarm_curves[i] = val; }

            v = cJSON_GetObjectItem(item, "color_temp_start");
            if (cJSON_IsNumber(v)) { config->alarm_color_temp_start[i] = (uint16_t)v->valuedouble; }

            v = cJSON_GetObjectItem(item, "name");
            if (cJSON_IsString(v)) {
                strncpy(config->alarm_names[i], v->valuestring, 15);
                config->alarm_names[i][15] = '\0';
            }
        }
    }

    // Animation presets — if present, replace all 5 slots
    cJSON *presets_arr = cJSON_GetObjectItem(json, "animation_presets");
    if (cJSON_IsArray(presets_arr)) {
        int count = cJSON_GetArraySize(presets_arr);
        if (count > ANIMATION_MAX_PRESETS) count = ANIMATION_MAX_PRESETS;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(presets_arr, i);
            animation_preset_t *p = &config->animation_presets[i];

            cJSON *v;
            v = cJSON_GetObjectItem(item, "name");
            if (cJSON_IsString(v)) {
                strncpy(p->name, v->valuestring, sizeof(p->name) - 1);
                p->name[sizeof(p->name) - 1] = '\0';
            }

            v = cJSON_GetObjectItem(item, "wavelength");
            if (cJSON_IsNumber(v)) {
                float val = (float)v->valuedouble;
                if (val >= 2.0f && val <= 300.0f) p->wavelength = val;
            }

            v = cJSON_GetObjectItem(item, "amplitude");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 100) p->amplitude = val; }

            v = cJSON_GetObjectItem(item, "speed");
            if (cJSON_IsNumber(v)) {
                float val = (float)v->valuedouble;
                if (val >= 0.0f && val <= 5.0f) p->speed = val;
            }

            v = cJSON_GetObjectItem(item, "base_brightness");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 100) p->base_brightness = val; }

            v = cJSON_GetObjectItem(item, "variation");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 100) p->variation = val; }

            v = cJSON_GetObjectItem(item, "color_temp");
            if (cJSON_IsNumber(v)) { uint16_t val = (uint16_t)v->valuedouble; if (val >= 2000 && val <= 6500) p->color_temp = val; }

            v = cJSON_GetObjectItem(item, "waveform");
            if (cJSON_IsNumber(v)) {
                int val = (int)v->valuedouble;
                config->anim_waveform[i] = (val == WAVE_SHAPE_HYPERBOLIC_AC || val == WAVE_SHAPE_HYPERBOLIC_PULSE) ? (uint8_t)val : WAVE_SHAPE_SINE;
            }

            v = cJSON_GetObjectItem(item, "wave_k");
            if (cJSON_IsNumber(v)) {
                int val = (int)(v->valuedouble * 10.0 + 0.5);
                if (val >= 5 && val <= 200) config->anim_wave_k_x10[i] = (uint8_t)val;
            }

            v = cJSON_GetObjectItem(item, "wave_i");
            if (cJSON_IsNumber(v)) {
                int val = (int)(v->valuedouble * 10.0 + 0.5);
                if (val >= 2 && val <= 40) config->anim_wave_i_x10[i] = (uint8_t)val;
            }
        }
    }

    // Dark mode — if present, replace all 3 slots
    cJSON *dark_arr = cJSON_GetObjectItem(json, "dark_mode");
    if (cJSON_IsArray(dark_arr)) {
        memset(config->dark_schedules, 0, sizeof(config->dark_schedules));
        int count = cJSON_GetArraySize(dark_arr);
        if (count > DARK_MODE_MAX_SCHEDULES) count = DARK_MODE_MAX_SCHEDULES;
        for (int i = 0; i < count; i++) {
            cJSON *item = cJSON_GetArrayItem(dark_arr, i);
            dark_mode_schedule_t *s = &config->dark_schedules[i];

            cJSON *v;
            v = cJSON_GetObjectItem(item, "enabled");
            if (cJSON_IsBool(v)) s->enabled = cJSON_IsTrue(v);

            v = cJSON_GetObjectItem(item, "start_hour");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 23) s->start_hour = val; }

            v = cJSON_GetObjectItem(item, "start_minute");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 59) s->start_minute = val; }

            v = cJSON_GetObjectItem(item, "end_hour");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 23) s->end_hour = val; }

            v = cJSON_GetObjectItem(item, "end_minute");
            if (cJSON_IsNumber(v)) { uint8_t val = (uint8_t)v->valuedouble; if (val <= 59) s->end_minute = val; }

            v = cJSON_GetObjectItem(item, "days_mask");
            if (cJSON_IsNumber(v)) s->days_mask = (uint8_t)v->valuedouble;

            v = cJSON_GetObjectItem(item, "allow_override");
            if (cJSON_IsBool(v)) s->allow_override = cJSON_IsTrue(v);
        }
    }

    // MQTT — if present, import settings (password not included in export)
    cJSON *mqtt_obj = cJSON_GetObjectItem(json, "mqtt");
    if (cJSON_IsObject(mqtt_obj)) {
        cJSON *v;
        v = cJSON_GetObjectItem(mqtt_obj, "enabled");
        if (cJSON_IsBool(v)) config->mqtt.enabled = cJSON_IsTrue(v);

        v = cJSON_GetObjectItem(mqtt_obj, "broker_uri");
        if (cJSON_IsString(v)) {
            strncpy(config->mqtt.broker_uri, v->valuestring, sizeof(config->mqtt.broker_uri) - 1);
            config->mqtt.broker_uri[sizeof(config->mqtt.broker_uri) - 1] = '\0';
        }

        v = cJSON_GetObjectItem(mqtt_obj, "username");
        if (cJSON_IsString(v)) {
            strncpy(config->mqtt.username, v->valuestring, sizeof(config->mqtt.username) - 1);
            config->mqtt.username[sizeof(config->mqtt.username) - 1] = '\0';
        }

        v = cJSON_GetObjectItem(mqtt_obj, "topic_prefix");
        if (cJSON_IsString(v) && strlen(v->valuestring) > 0) {
            strncpy(config->mqtt.topic_prefix, v->valuestring, sizeof(config->mqtt.topic_prefix) - 1);
            config->mqtt.topic_prefix[sizeof(config->mqtt.topic_prefix) - 1] = '\0';
        }

        v = cJSON_GetObjectItem(mqtt_obj, "device_name");
        if (cJSON_IsString(v) && strlen(v->valuestring) > 0) {
            strncpy(config->mqtt.device_name, v->valuestring, sizeof(config->mqtt.device_name) - 1);
            config->mqtt.device_name[sizeof(config->mqtt.device_name) - 1] = '\0';
        }
    }

    config_manager_save();
    sunrise_engine_update_schedule();

    // Restart MQTT if config changed
    if (config->mqtt.enabled) {
        mqtt_manager_start();
    } else {
        mqtt_manager_stop();
    }

    cJSON_Delete(json);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddBoolToObject(resp, "restart_required", restart_required);
    return send_json_response(req, resp);
}

// GET /api/mqtt
static esp_err_t api_mqtt_get_handler(httpd_req_t *req)
{
    device_config_t *config = config_manager_get();
    cJSON *json = cJSON_CreateObject();

    cJSON_AddBoolToObject(json, "enabled", config->mqtt.enabled);
    cJSON_AddStringToObject(json, "broker_uri", config->mqtt.broker_uri);
    cJSON_AddStringToObject(json, "username", config->mqtt.username);
    cJSON_AddBoolToObject(json, "password_set", config->mqtt.password[0] != '\0');
    cJSON_AddStringToObject(json, "topic_prefix", config->mqtt.topic_prefix);
    cJSON_AddStringToObject(json, "device_name", config->mqtt.device_name);
    cJSON_AddBoolToObject(json, "connected", mqtt_manager_is_connected());

    return send_json_response(req, json);
}

// POST /api/mqtt
static esp_err_t api_mqtt_post_handler(httpd_req_t *req)
{
    cJSON *json = parse_json_body(req);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    device_config_t *config = config_manager_get();

    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(enabled)) config->mqtt.enabled = cJSON_IsTrue(enabled);

    cJSON *broker = cJSON_GetObjectItem(json, "broker_uri");
    if (cJSON_IsString(broker)) {
        strncpy(config->mqtt.broker_uri, broker->valuestring, sizeof(config->mqtt.broker_uri) - 1);
        config->mqtt.broker_uri[sizeof(config->mqtt.broker_uri) - 1] = '\0';
    }

    cJSON *username = cJSON_GetObjectItem(json, "username");
    if (cJSON_IsString(username)) {
        strncpy(config->mqtt.username, username->valuestring, sizeof(config->mqtt.username) - 1);
        config->mqtt.username[sizeof(config->mqtt.username) - 1] = '\0';
    }

    cJSON *password = cJSON_GetObjectItem(json, "password");
    if (cJSON_IsString(password) && strlen(password->valuestring) > 0) {
        strncpy(config->mqtt.password, password->valuestring, sizeof(config->mqtt.password) - 1);
        config->mqtt.password[sizeof(config->mqtt.password) - 1] = '\0';
    }

    cJSON *prefix = cJSON_GetObjectItem(json, "topic_prefix");
    if (cJSON_IsString(prefix) && strlen(prefix->valuestring) > 0) {
        strncpy(config->mqtt.topic_prefix, prefix->valuestring, sizeof(config->mqtt.topic_prefix) - 1);
        config->mqtt.topic_prefix[sizeof(config->mqtt.topic_prefix) - 1] = '\0';
    }

    cJSON *name = cJSON_GetObjectItem(json, "device_name");
    if (cJSON_IsString(name) && strlen(name->valuestring) > 0) {
        strncpy(config->mqtt.device_name, name->valuestring, sizeof(config->mqtt.device_name) - 1);
        config->mqtt.device_name[sizeof(config->mqtt.device_name) - 1] = '\0';
    }

    config_manager_save();
    cJSON_Delete(json);

    // Start or stop MQTT based on new config
    if (config->mqtt.enabled) {
        mqtt_manager_start();
    } else {
        mqtt_manager_stop();
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    return send_json_response(req, resp);
}

// POST /api/factory-reset
static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested via API");

    device_config_t *config = config_manager_get();

    // Preserve WiFi credentials and setup flag
    wifi_config_stored_t saved_wifi;
    memcpy(&saved_wifi, &config->wifi, sizeof(saved_wifi));
    bool saved_setup = config->setup_complete;

    // Erase NVS and reset all defaults
    config_manager_factory_reset();

    // Restore WiFi credentials and setup flag
    memcpy(&config->wifi, &saved_wifi, sizeof(saved_wifi));
    config->setup_complete = saved_setup;
    config_manager_save();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    send_json_response(req, resp);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// POST /api/reboot
static esp_err_t api_reboot_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    send_json_response(req, resp);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (server != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting web server");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 26;
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;  // Reduced to avoid socket exhaustion

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Static files
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t style = { .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler };
    httpd_uri_t app_js = { .uri = "/app.js", .method = HTTP_GET, .handler = app_js_get_handler };

    // API endpoints
    httpd_uri_t api_status = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_handler };
    httpd_uri_t api_wifi_scan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = api_wifi_scan_handler };
    httpd_uri_t api_wifi_connect = { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = api_wifi_connect_handler };
    httpd_uri_t api_config_get = { .uri = "/api/config", .method = HTTP_GET, .handler = api_config_get_handler };
    httpd_uri_t api_config_post = { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler };
    httpd_uri_t api_alarms_get = { .uri = "/api/alarms", .method = HTTP_GET, .handler = api_alarms_get_handler };
    httpd_uri_t api_alarms_post = { .uri = "/api/alarms", .method = HTTP_POST, .handler = api_alarms_post_handler };
    httpd_uri_t api_led_test = { .uri = "/api/led/test", .method = HTTP_POST, .handler = api_led_test_handler };
    httpd_uri_t api_time_sync = { .uri = "/api/time/sync", .method = HTTP_POST, .handler = api_time_sync_handler };

    // Animation endpoints
    httpd_uri_t api_anim_presets_get = { .uri = "/api/animation/presets", .method = HTTP_GET, .handler = api_animation_presets_get_handler };
    httpd_uri_t api_anim_presets_post = { .uri = "/api/animation/presets", .method = HTTP_POST, .handler = api_animation_presets_post_handler };
    httpd_uri_t api_anim_start = { .uri = "/api/animation/start", .method = HTTP_POST, .handler = api_animation_start_handler };
    httpd_uri_t api_anim_stop = { .uri = "/api/animation/stop", .method = HTTP_POST, .handler = api_animation_stop_handler };

    // Dark mode endpoints
    httpd_uri_t api_darkmode_get = { .uri = "/api/darkmode", .method = HTTP_GET, .handler = api_darkmode_get_handler };
    httpd_uri_t api_darkmode_post = { .uri = "/api/darkmode", .method = HTTP_POST, .handler = api_darkmode_post_handler };

    // MQTT endpoints
    httpd_uri_t api_mqtt_get = { .uri = "/api/mqtt", .method = HTTP_GET, .handler = api_mqtt_get_handler };
    httpd_uri_t api_mqtt_post = { .uri = "/api/mqtt", .method = HTTP_POST, .handler = api_mqtt_post_handler };

    // Config export/import and reboot
    httpd_uri_t api_config_export = { .uri = "/api/config/export", .method = HTTP_GET, .handler = api_config_export_handler };
    httpd_uri_t api_config_import = { .uri = "/api/config/import", .method = HTTP_POST, .handler = api_config_import_handler };
    httpd_uri_t api_factory_reset = { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = api_factory_reset_handler };
    httpd_uri_t api_reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = api_reboot_handler };

    // CORS preflight
    httpd_uri_t options = { .uri = "/api/*", .method = HTTP_OPTIONS, .handler = options_handler };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &style);
    httpd_register_uri_handler(server, &app_js);
    httpd_register_uri_handler(server, &api_status);
    httpd_register_uri_handler(server, &api_wifi_scan);
    httpd_register_uri_handler(server, &api_wifi_connect);
    httpd_register_uri_handler(server, &api_config_get);
    httpd_register_uri_handler(server, &api_config_post);
    httpd_register_uri_handler(server, &api_alarms_get);
    httpd_register_uri_handler(server, &api_alarms_post);
    httpd_register_uri_handler(server, &api_led_test);
    httpd_register_uri_handler(server, &api_time_sync);
    httpd_register_uri_handler(server, &api_anim_presets_get);
    httpd_register_uri_handler(server, &api_anim_presets_post);
    httpd_register_uri_handler(server, &api_anim_start);
    httpd_register_uri_handler(server, &api_anim_stop);
    httpd_register_uri_handler(server, &api_darkmode_get);
    httpd_register_uri_handler(server, &api_darkmode_post);
    httpd_register_uri_handler(server, &api_mqtt_get);
    httpd_register_uri_handler(server, &api_mqtt_post);
    httpd_register_uri_handler(server, &api_config_export);
    httpd_register_uri_handler(server, &api_config_import);
    httpd_register_uri_handler(server, &api_factory_reset);
    httpd_register_uri_handler(server, &api_reboot);
    httpd_register_uri_handler(server, &options);

    // 404 handler for captive portal
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_handler);

    // Start DNS server for captive portal
    dns_server_start();

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (server == NULL) {
        return ESP_OK;
    }

    dns_server_stop();
    httpd_stop(server);
    server = NULL;

    ESP_LOGI(TAG, "Web server stopped");
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return server != NULL;
}
