#ifndef ANIMATION_ENGINE_H
#define ANIMATION_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "animation_preset.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the animation engine
 *
 * Sets up internal state but does not start any animation.
 *
 * @return ESP_OK on success
 */
esp_err_t animation_engine_init(void);

/**
 * @brief Start animation with specified preset
 *
 * Stops any running sunrise or animation, then starts the wave animation
 * using the specified preset. For WS2811 LEDs, creates a true spatial wave.
 * For PWM LEDs, creates a temporal wave (all LEDs pulse together).
 *
 * @param preset_id Preset index (0 to ANIMATION_MAX_PRESETS-1)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if preset_id invalid
 */
esp_err_t animation_engine_start(uint8_t preset_id);

/**
 * @brief Stop the running animation
 *
 * Turns off LEDs and stops the animation task.
 */
void animation_engine_stop(void);

/**
 * @brief Check if animation is currently running
 *
 * @return true if animation task is active
 */
bool animation_engine_is_running(void);

/**
 * @brief Get active preset ID
 *
 * @return Active preset index (0-4) or -1 if no animation running
 */
int8_t animation_engine_get_active_preset(void);

/**
 * @brief Get preset by ID
 *
 * Returns a pointer to the preset stored in config_manager.
 *
 * @param id Preset index (0 to ANIMATION_MAX_PRESETS-1)
 * @return Pointer to preset, or NULL if invalid ID
 */
animation_preset_t *animation_engine_get_preset(uint8_t id);

/**
 * @brief Save preset to config
 *
 * Updates the preset at the specified index and saves to NVS.
 *
 * @param id Preset index (0 to ANIMATION_MAX_PRESETS-1)
 * @param preset Preset data to save
 * @return ESP_OK on success
 */
esp_err_t animation_engine_save_preset(uint8_t id, const animation_preset_t *preset);

/**
 * @brief Deinitialize the animation engine
 *
 * Stops any running animation and cleans up resources.
 */
void animation_engine_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_ENGINE_H
