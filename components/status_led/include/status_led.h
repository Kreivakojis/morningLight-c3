#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Status LED blink patterns
 */
typedef enum {
    STATUS_LED_PATTERN_OFF,          // LED off
    STATUS_LED_PATTERN_ON,           // LED solid on
    STATUS_LED_PATTERN_SLOW_BLINK,   // 1 Hz blink (connecting/disconnected)
    STATUS_LED_PATTERN_FAST_BLINK,   // 4 Hz blink (AP mode/config)
    STATUS_LED_PATTERN_DOUBLE_BLINK, // Double blink pattern (alarm active)
    STATUS_LED_PATTERN_TRIPLE_BLINK, // Triple blink pattern (error)
    STATUS_LED_PATTERN_CONNECTED,    // Brief flash every 5 seconds (connected)
    STATUS_LED_PATTERN_SUNRISE,      // Slow pulse (sunrise in progress)
} status_led_pattern_t;

/**
 * @brief Initialize the status LED
 *
 * @return ESP_OK on success
 */
esp_err_t status_led_init(void);

/**
 * @brief Set the current blink pattern
 *
 * @param pattern The pattern to display
 */
void status_led_set_pattern(status_led_pattern_t pattern);

/**
 * @brief Get the current blink pattern
 *
 * @return Current pattern
 */
status_led_pattern_t status_led_get_pattern(void);

/**
 * @brief Turn status LED on
 */
void status_led_on(void);

/**
 * @brief Turn status LED off
 */
void status_led_off(void);

/**
 * @brief Toggle status LED state
 */
void status_led_toggle(void);

/**
 * @brief Deinitialize the status LED
 */
void status_led_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // STATUS_LED_H
