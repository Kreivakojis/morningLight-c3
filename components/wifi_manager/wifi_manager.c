#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "wifi_manager.h"
#include "config_manager.h"

// Forward declare app events (defined in main)
extern esp_event_base_t APP_EVENTS;
enum {
    APP_EVENT_WIFI_CONNECTED = 0,
    APP_EVENT_WIFI_DISCONNECTED,
    APP_EVENT_WIFI_AP_STARTED,
    APP_EVENT_WIFI_AP_STOPPED,
    APP_EVENT_WIFI_GOT_IP,
};

static const char *TAG = "wifi_mgr";

// Event bits
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1
#define WIFI_SCAN_DONE_BIT    BIT2

// State
static struct {
    bool initialized;
    bool wifi_started;
    wifi_mgr_state_t state;
    EventGroupHandle_t event_group;
    esp_netif_t *netif_sta;
    esp_netif_t *netif_ap;
    uint32_t ip_addr;
    int retry_count;
    TimerHandle_t fallback_timer;
    bool scan_in_progress;
    bool scan_complete;
    wifi_ap_record_t *scan_results;
    uint16_t scan_count;
    char ap_ssid[33];
} state = {0};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "STA connected to AP");
                state.retry_count = 0;
                if (state.fallback_timer) {
                    xTimerStop(state.fallback_timer, 0);
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGI(TAG, "Disconnected from AP, reason: %d", event->reason);

                if (state.state == WIFI_MGR_STATE_STA_CONNECTING ||
                    state.state == WIFI_MGR_STATE_STA_CONNECTED) {
                    state.retry_count++;
                    if (state.retry_count < CONFIG_ML_WIFI_STA_RETRY_COUNT) {
                        ESP_LOGI(TAG, "Retry %d/%d", state.retry_count, CONFIG_ML_WIFI_STA_RETRY_COUNT);
                        esp_wifi_connect();
                    } else {
                        ESP_LOGW(TAG, "Max retries reached, falling back to AP mode");
                        xEventGroupSetBits(state.event_group, WIFI_FAIL_BIT);
                        wifi_manager_start_ap();
                        state.state = WIFI_MGR_STATE_AP_STA;
                    }
                }

                state.ip_addr = 0;
                esp_event_post(APP_EVENTS, APP_EVENT_WIFI_DISCONNECTED, NULL, 0, 0);
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started: %s", state.ap_ssid);
                if (state.state == WIFI_MGR_STATE_IDLE) {
                    state.state = WIFI_MGR_STATE_AP_ONLY;
                }
                esp_event_post(APP_EVENTS, APP_EVENT_WIFI_AP_STARTED, NULL, 0, 0);
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                esp_event_post(APP_EVENTS, APP_EVENT_WIFI_AP_STOPPED, NULL, 0, 0);
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station " MACSTR " joined AP", MAC2STR(event->mac));
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station " MACSTR " left AP", MAC2STR(event->mac));
                break;
            }

            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "Scan complete");
                state.scan_in_progress = false;
                state.scan_complete = true;
                xEventGroupSetBits(state.event_group, WIFI_SCAN_DONE_BIT);
                break;

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            state.ip_addr = event->ip_info.ip.addr;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

            state.state = WIFI_MGR_STATE_STA_CONNECTED;
            xEventGroupSetBits(state.event_group, WIFI_CONNECTED_BIT);

            esp_event_post(APP_EVENTS, APP_EVENT_WIFI_GOT_IP, &state.ip_addr, sizeof(state.ip_addr), 0);
            esp_event_post(APP_EVENTS, APP_EVENT_WIFI_CONNECTED, NULL, 0, 0);
        }
    }
}

static void fallback_timer_callback(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "STA connection timeout, starting AP mode");
    wifi_manager_start_ap();
    state.state = WIFI_MGR_STATE_AP_STA;
}

static void generate_ap_ssid(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(state.ap_ssid, sizeof(state.ap_ssid), "%s-%02X%02X",
             CONFIG_ML_WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi manager");

    if (state.initialized) {
        return ESP_OK;
    }

    // Create event group
    state.event_group = xEventGroupCreate();
    if (state.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default network interfaces
    state.netif_sta = esp_netif_create_default_wifi_sta();
    state.netif_ap = esp_netif_create_default_wifi_ap();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Disable WiFi modem-sleep power save. Classic ESP32's RMT peripheral has
    // no DMA path (DMA-capable RMT is S3+ only) and only 64 words of hardware
    // buffer per channel, so it depends on a timely interrupt-driven refill
    // to keep up with the WS2811 bit stream. Modem-sleep's periodic MAC/PHY
    // wake transitions disable interrupts for long enough to miss that
    // deadline, corrupting individual pixels on the addressable strips —
    // confirmed by A/B testing with WiFi fully killed at runtime (no
    // corruption) vs. connected (corruption present). This trades a small
    // amount of extra power draw for correct LED output.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL));

    // Generate AP SSID
    generate_ap_ssid();

    // Create fallback timer
    state.fallback_timer = xTimerCreate("wifi_fallback",
                                        pdMS_TO_TICKS(CONFIG_ML_WIFI_FALLBACK_TIMEOUT_S * 1000),
                                        pdFALSE, NULL, fallback_timer_callback);

    state.initialized = true;
    state.state = WIFI_MGR_STATE_IDLE;

    // Check if we have stored WiFi credentials
    device_config_t *config = config_manager_get();
    if (config && config->setup_complete && strlen(config->wifi.ssid) > 0) {
        ESP_LOGI(TAG, "Found stored WiFi credentials, connecting to %s", config->wifi.ssid);
        wifi_manager_connect(config->wifi.ssid, config->wifi.password);
        // Start fallback timer
        xTimerStart(state.fallback_timer, 0);
    } else {
        ESP_LOGI(TAG, "No WiFi credentials, starting AP mode");
        wifi_manager_start_ap();
    }

    return ESP_OK;
}

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return state.state;
}

esp_err_t wifi_manager_start_scan(void)
{
    if (!state.initialized) return ESP_ERR_INVALID_STATE;
    if (state.scan_in_progress) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Starting WiFi scan");

    // Scanning requires STA mode - switch to APSTA if currently AP only
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&current_mode);
    if (current_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Switching to APSTA mode for scan");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
            return err;
        }
    }

    // Free previous results
    if (state.scan_results) {
        free(state.scan_results);
        state.scan_results = NULL;
    }
    state.scan_count = 0;
    state.scan_complete = false;
    state.scan_in_progress = true;

    xEventGroupClearBits(state.event_group, WIFI_SCAN_DONE_BIT);

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        state.scan_in_progress = false;
    }
    return err;
}

uint16_t wifi_manager_get_scan_results(wifi_scan_result_t *results, uint16_t max_results)
{
    if (!state.scan_complete || results == NULL) return 0;

    // Get number of APs found
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) return 0;

    // Allocate buffer for results
    wifi_ap_record_t *ap_records = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) return 0;

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    // Copy to output format
    uint16_t count = (ap_count < max_results) ? ap_count : max_results;
    for (uint16_t i = 0; i < count; i++) {
        strncpy(results[i].ssid, (char *)ap_records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
        results[i].rssi = ap_records[i].rssi;
        results[i].authmode = ap_records[i].authmode;
    }

    free(ap_records);
    return count;
}

bool wifi_manager_scan_complete(void)
{
    return state.scan_complete;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!state.initialized) return ESP_ERR_INVALID_STATE;
    if (ssid == NULL || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Connecting to %s", ssid);

    state.state = WIFI_MGR_STATE_STA_CONNECTING;
    state.retry_count = 0;

    // Configure WiFi STA
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Set mode (keep AP if it's running)
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&current_mode);
    if (current_mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Start WiFi if not already started
    if (!state.wifi_started) {
        ESP_LOGI(TAG, "Starting WiFi");
        ESP_ERROR_CHECK(esp_wifi_start());
        state.wifi_started = true;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!state.initialized) return ESP_ERR_INVALID_STATE;

    state.retry_count = CONFIG_ML_WIFI_STA_RETRY_COUNT; // Prevent reconnect
    return esp_wifi_disconnect();
}

bool wifi_manager_is_connected(void)
{
    return state.state == WIFI_MGR_STATE_STA_CONNECTED;
}

uint32_t wifi_manager_get_ip(void)
{
    return state.ip_addr;
}

void wifi_manager_get_ap_ssid(char *ssid)
{
    if (ssid) {
        strcpy(ssid, state.ap_ssid);
    }
}

esp_err_t wifi_manager_start_ap(void)
{
    if (!state.initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Starting AP mode: %s", state.ap_ssid);

    // Configure AP
    wifi_config_t wifi_config = {0};
    wifi_config.ap.channel = CONFIG_ML_WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = CONFIG_ML_WIFI_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.pmf_cfg.required = false;
    wifi_config.ap.ssid_hidden = 0;
    strncpy((char *)wifi_config.ap.ssid, state.ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(state.ap_ssid);

    // Set mode (keep STA if connecting)
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_STA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else if (current_mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    // Start WiFi if not already started
    if (!state.wifi_started) {
        ESP_LOGI(TAG, "Starting WiFi");
        ESP_ERROR_CHECK(esp_wifi_start());
        state.wifi_started = true;
    }

    if (state.state == WIFI_MGR_STATE_IDLE) {
        state.state = WIFI_MGR_STATE_AP_ONLY;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(void)
{
    if (!state.initialized) return ESP_ERR_INVALID_STATE;

    wifi_mode_t current_mode;
    esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    } else if (current_mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_stop());
        state.state = WIFI_MGR_STATE_IDLE;
    }

    return ESP_OK;
}

void wifi_manager_deinit(void)
{
    if (!state.initialized) return;

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);

    esp_wifi_stop();
    esp_wifi_deinit();

    if (state.fallback_timer) {
        xTimerDelete(state.fallback_timer, 0);
    }

    if (state.event_group) {
        vEventGroupDelete(state.event_group);
    }

    if (state.scan_results) {
        free(state.scan_results);
    }

    esp_netif_destroy(state.netif_sta);
    esp_netif_destroy(state.netif_ap);

    memset(&state, 0, sizeof(state));
}
