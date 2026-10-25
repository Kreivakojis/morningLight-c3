#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi manager state
 */
typedef enum {
    WIFI_MGR_STATE_IDLE,
    WIFI_MGR_STATE_AP_ONLY,
    WIFI_MGR_STATE_STA_CONNECTING,
    WIFI_MGR_STATE_STA_CONNECTED,
    WIFI_MGR_STATE_AP_STA,  // Fallback mode
} wifi_mgr_state_t;

/**
 * @brief WiFi scan result entry
 */
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_scan_result_t;

/**
 * @brief Initialize WiFi manager
 *
 * Starts WiFi based on stored configuration:
 * - If WiFi credentials exist: STA mode
 * - Otherwise: AP mode for configuration
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_init(void);

/**
 * @brief Get current WiFi state
 *
 * @return Current state
 */
wifi_mgr_state_t wifi_manager_get_state(void);

/**
 * @brief Start WiFi scan
 *
 * Scan runs asynchronously. Results available via wifi_manager_get_scan_results().
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_start_scan(void);

/**
 * @brief Get scan results
 *
 * @param results Buffer to store results
 * @param max_results Maximum number of results to return
 * @return Number of results stored
 */
uint16_t wifi_manager_get_scan_results(wifi_scan_result_t *results, uint16_t max_results);

/**
 * @brief Check if scan is complete
 *
 * @return true if scan results are available
 */
bool wifi_manager_scan_complete(void);

/**
 * @brief Connect to WiFi network
 *
 * @param ssid Network SSID
 * @param password Network password (NULL for open networks)
 * @return ESP_OK if connection started
 */
esp_err_t wifi_manager_connect(const char *ssid, const char *password);

/**
 * @brief Disconnect from current network
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_disconnect(void);

/**
 * @brief Check if connected to STA network
 *
 * @return true if connected
 */
bool wifi_manager_is_connected(void);

/**
 * @brief Get current IP address
 *
 * @return IP address as uint32_t, 0 if not connected
 */
uint32_t wifi_manager_get_ip(void);

/**
 * @brief Get AP SSID (including MAC suffix)
 *
 * @param ssid Buffer to store SSID (min 33 bytes)
 */
void wifi_manager_get_ap_ssid(char *ssid);

/**
 * @brief Start AP mode explicitly
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_start_ap(void);

/**
 * @brief Stop AP mode
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_manager_stop_ap(void);

/**
 * @brief Deinitialize WiFi manager
 */
void wifi_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
