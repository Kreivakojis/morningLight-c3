#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the captive portal DNS server
 *
 * Redirects all DNS queries to the AP IP address for captive portal functionality.
 *
 * @return ESP_OK on success
 */
esp_err_t dns_server_start(void);

/**
 * @brief Stop the DNS server
 *
 * @return ESP_OK on success
 */
esp_err_t dns_server_stop(void);

/**
 * @brief Check if DNS server is running
 *
 * @return true if running
 */
bool dns_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif // DNS_SERVER_H
