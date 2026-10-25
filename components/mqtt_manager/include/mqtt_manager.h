#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MQTT manager
 *
 * Sets up internal state. Auto-starts if MQTT is enabled in config.
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_init(void);

/**
 * @brief Start or restart MQTT client with current config
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_start(void);

/**
 * @brief Stop MQTT client and disconnect
 *
 * @return ESP_OK on success
 */
esp_err_t mqtt_manager_stop(void);

/**
 * @brief Check if MQTT is connected
 *
 * @return true if connected to broker
 */
bool mqtt_manager_is_connected(void);

/**
 * @brief Force publish all entity state
 */
void mqtt_manager_publish_state(void);

/**
 * @brief Notify that light state changed (triggers state publish)
 */
void mqtt_manager_notify_light_changed(void);

/**
 * @brief Re-publish Home Assistant discovery payloads (e.g. after renaming sensors)
 */
void mqtt_manager_republish_discovery(void);

/**
 * @brief Deinitialize MQTT manager
 */
void mqtt_manager_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_MANAGER_H
