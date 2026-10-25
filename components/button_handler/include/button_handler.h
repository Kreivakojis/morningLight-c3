#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button IDs
 */
typedef enum {
    BUTTON_RESET = 0,    // Boot button (GPIO 0)
    BUTTON_SCENARIO,     // Scenario button
    BUTTON_MAX,
} button_id_t;

/**
 * @brief Button event types
 */
typedef enum {
    BUTTON_EVENT_PRESSED,       // Button pressed
    BUTTON_EVENT_RELEASED,      // Button released
    BUTTON_EVENT_SHORT_PRESS,   // Short press (<1s)
    BUTTON_EVENT_LONG_PRESS,    // Long press (1-5s)
    BUTTON_EVENT_VERY_LONG,     // Very long press (>5s) - factory reset
} button_event_type_t;

/**
 * @brief Initialize the button handler
 *
 * @return ESP_OK on success
 */
esp_err_t button_handler_init(void);

/**
 * @brief Check if button is currently pressed
 *
 * @param button Button ID
 * @return true if pressed
 */
bool button_handler_is_pressed(button_id_t button);

/**
 * @brief Get time since button was pressed
 *
 * @param button Button ID
 * @return Milliseconds since press, 0 if not pressed
 */
uint32_t button_handler_press_duration(button_id_t button);

/**
 * @brief Deinitialize button handler
 */
void button_handler_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_HANDLER_H
