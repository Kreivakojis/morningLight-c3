#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web server
 *
 * Starts HTTP server with REST API endpoints and serves embedded web UI.
 * Also starts DNS server for captive portal when in AP mode.
 *
 * @return ESP_OK on success
 */
esp_err_t web_server_start(void);

/**
 * @brief Stop the web server
 *
 * @return ESP_OK on success
 */
esp_err_t web_server_stop(void);

/**
 * @brief Check if web server is running
 *
 * @return true if running
 */
bool web_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
