#ifndef ANIMATION_PRESET_H
#define ANIMATION_PRESET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANIMATION_MAX_PRESETS 5

/**
 * @brief Animation preset configuration
 *
 * Defines parameters for a parametric sine-wave animation.
 * Total size: 24 bytes per preset
 */
typedef struct {
    char name[12];           /**< User-friendly name (null-terminated) */
    float wavelength;        /**< 2.0 - 300.0 LEDs (distance between peaks) */
    uint8_t amplitude;       /**< 0-100% (contrast between peaks/valleys) */
    float speed;             /**< 0.0 - 5.0 cycles/sec */
    uint8_t base_brightness; /**< 0-100% (midpoint brightness) */
    uint8_t variation;       /**< 0-100% (organic noise amount) */
    uint16_t color_temp;     /**< 2000-6500K */
} animation_preset_t;

/**
 * @brief Waveform shape enum, shared between wave_generator.c and its callers
 */
typedef enum {
    WAVE_SHAPE_SINE = 0,
    WAVE_SHAPE_HYPERBOLIC_AC = 1,
    WAVE_SHAPE_HYPERBOLIC_PULSE = 2,  /**< Hyperbolic AC, full-wave rectified (no negative lobe) */
} wave_shape_kind_t;

/**
 * @brief Raw wave shape, resolved from config_manager's per-preset
 * anim_waveform/anim_wave_k_x10/anim_wave_i_x10 arrays (kept separate from
 * animation_preset_t, since that struct is embedded mid-struct in
 * device_config_t and can't grow without breaking NVS compat for every
 * field declared after it)
 */
typedef struct {
    uint8_t kind;  /**< wave_shape_kind_t */
    float k;       /**< Hyperbolic AC: zero-crossing sharpness, 0.5-20.0 */
    float i;       /**< Hyperbolic AC: crest shape, 0.2-4.0 (2.0 = unity peak) */
} wave_shape_t;

#ifdef __cplusplus
}
#endif

#endif // ANIMATION_PRESET_H
