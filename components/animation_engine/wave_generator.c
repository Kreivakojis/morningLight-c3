#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "animation_preset.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/**
 * @brief Simple interpolated value noise (deterministic, smooth)
 *
 * Uses a hash-based approach for pseudo-random values with
 * cubic Hermite interpolation for smoothness.
 *
 * @param x Position to sample
 * @param seed Seed for variation
 * @return Noise value in range [0, 1]
 */
static float value_noise(float x, float seed)
{
    int xi = (int)floorf(x);
    float xf = x - xi;

    // Smooth interpolation (cubic Hermite)
    float t = xf * xf * (3.0f - 2.0f * xf);

    // Simple hash function for deterministic pseudo-random
    float h1 = sinf(xi * 12.9898f + seed * 78.233f) * 43758.5453f;
    float h2 = sinf((xi + 1) * 12.9898f + seed * 78.233f) * 43758.5453f;
    h1 = h1 - floorf(h1);
    h2 = h2 - floorf(h2);

    return h1 + (h2 - h1) * t;
}

/**
 * @brief Triangle wave equivalent to asinf(sinf(x)), without the trig calls
 *
 * asin(sin(x)) is a triangle wave oscillating between -pi/2 and +pi/2 with
 * period 2*pi. Reducing x into [-pi, pi] and folding around +-pi/2 gives the
 * exact same result as asinf(sinf(x)) for a fraction of the cost (no asinf,
 * no sinf) -- matters here since it's evaluated per-LED, per-frame.
 *
 * @param x Phase angle (radians), any range
 * @return Triangle wave value in [-pi/2, pi/2]
 */
static float triangle_wave(float x)
{
    // Reduce to [-pi, pi]
    float r = fmodf(x + (float)M_PI, 2.0f * (float)M_PI);
    if (r < 0.0f) r += 2.0f * (float)M_PI;
    r -= (float)M_PI;

    if (r > (float)M_PI / 2.0f) {
        return (float)M_PI - r;
    } else if (r < -(float)M_PI / 2.0f) {
        return -(float)M_PI - r;
    }
    return r;
}

/**
 * @brief Hyperbolic AC wave: sinh(k * (i/pi) * asin(sin(x))) / sinh(k)
 *
 * Models a peaky, rectified-AC-like wave: near-flat/near-zero dwell between
 * zero crossings, punctuated by sharp alternating spikes. `k` controls how
 * sharply the spike pinches in (near 0 it degenerates toward a plain sine;
 * large k gives a narrow spike with long flat zero regions). `i` controls
 * crest height via the sinh nonlinearity -- i=2.0 normalizes the peak to
 * exactly 1.0 regardless of k; above 2.0 peaks overshoot past 1.0 and will
 * hard-clip against the brightness ceiling (a deliberate "flash" look).
 *
 * @param x Phase angle (radians)
 * @param k Zero-crossing sharpness, must be > 0
 * @param i Crest shape
 * @param inv_sinh_k Precomputed 1.0f / sinhf(k) (shared across all LEDs/frame)
 * @return Wave value, nominally in [-1, 1] but can exceed it when i > 2.0
 */
static inline float hyperbolic_ac_wave(float x, float k, float i, float inv_sinh_k)
{
    float t = triangle_wave(x);
    return sinhf(k * (i / (float)M_PI) * t) * inv_sinh_k;
}

/**
 * @brief Resolve the raw wave value (before amplitude/base/variation) for a phase
 *
 * Shared by both compute entry points. Hyperbolic Pulse is the same
 * hyperbolic AC shape, full-wave rectified (absolute value) so every zero
 * crossing produces an upward pulse instead of alternating up/down spikes.
 */
static inline float resolve_wave(float phase, const wave_shape_t *shape, float hk, float hi, float inv_sinh_k)
{
    if (!shape) {
        return sinf(phase);
    }
    switch (shape->kind) {
        case WAVE_SHAPE_HYPERBOLIC_AC:
            return hyperbolic_ac_wave(phase, hk, hi, inv_sinh_k);
        case WAVE_SHAPE_HYPERBOLIC_PULSE:
            return fabsf(hyperbolic_ac_wave(phase, hk, hi, inv_sinh_k));
        case WAVE_SHAPE_SINE:
        default:
            return sinf(phase);
    }
}

/**
 * @brief Compute per-LED brightness values for the wave animation
 *
 * Core formula: brightness(x, t) = base + amplitude * wave(spatial + temporal) * variation
 * where wave() is either a plain sine or the hyperbolic AC shape, per `shape`.
 *
 * @param brightness_out Output array of brightness values (0-100)
 * @param led_count Number of LEDs to compute
 * @param preset Animation preset with parameters
 * @param time_sec Current time in seconds (for temporal animation)
 * @param shape Waveform shape to use (NULL defaults to plain sine)
 */
void wave_generator_compute(
    uint8_t *brightness_out,
    uint16_t led_count,
    const animation_preset_t *preset,
    float time_sec,
    const wave_shape_t *shape)
{
    if (!brightness_out || !preset || led_count == 0) {
        return;
    }

    // Pre-calculate constants
    float spatial_freq = (2.0f * M_PI) / preset->wavelength;
    float temporal_phase = time_sec * preset->speed * 2.0f * M_PI;
    float amplitude = preset->amplitude / 100.0f;
    float base = preset->base_brightness / 100.0f;

    // Variation scales from 0-100% to 0-20% actual variation
    float variation_strength = preset->variation / 100.0f * 0.4f;

    bool use_hyperbolic = shape && (shape->kind == WAVE_SHAPE_HYPERBOLIC_AC || shape->kind == WAVE_SHAPE_HYPERBOLIC_PULSE);
    float hk = use_hyperbolic && shape->k > 0.01f ? shape->k : 0.01f;
    float hi = use_hyperbolic ? shape->i : 0.0f;
    float inv_sinh_k = use_hyperbolic ? 1.0f / sinhf(hk) : 0.0f;

    for (uint16_t i = 0; i < led_count; i++) {
        // Spatial phase based on LED position
        float spatial_phase = i * spatial_freq;
        float phase = spatial_phase + temporal_phase;

        float wave = resolve_wave(phase, shape, hk, hi, inv_sinh_k);

        // Organic variation using slow-moving noise
        float noise_x = i * 0.1f + time_sec * 0.3f;
        float variation = 1.0f + (value_noise(noise_x, 42.0f) - 0.5f) * 2.0f * variation_strength;

        // Combine: base + amplitude * wave * variation
        // Amplitude is halved because the wave is symmetric in [-1, 1]-ish
        // and we want symmetry around base
        float brightness = base + amplitude * 0.5f * wave * variation;

        // Clamp to valid range [0, 1]
        if (brightness < 0.0f) brightness = 0.0f;
        if (brightness > 1.0f) brightness = 1.0f;

        // Convert to percentage
        brightness_out[i] = (uint8_t)(brightness * 100.0f);
    }
}

/**
 * @brief Compute per-LED brightness values with base brightness override
 *
 * Same as wave_generator_compute() but allows overriding the base brightness.
 * Used for animated sunrise where base ramps from 0 to target over the duration.
 *
 * @param brightness_out Output array of brightness values (0-100)
 * @param led_count Number of LEDs to compute
 * @param preset Animation preset with parameters
 * @param time_sec Current time in seconds (for temporal animation)
 * @param base_override Base brightness override (0-100), or -1 to use preset value
 * @param amplitude_scale Scale factor for amplitude (0.0-1.0), negative to use full amplitude
 * @param shape Waveform shape to use (NULL defaults to plain sine)
 */
void wave_generator_compute_with_base(
    uint8_t *brightness_out,
    uint16_t led_count,
    const animation_preset_t *preset,
    float time_sec,
    int16_t base_override,
    float amplitude_scale,
    const wave_shape_t *shape)
{
    if (!brightness_out || !preset || led_count == 0) {
        return;
    }

    // Pre-calculate constants
    float spatial_freq = (2.0f * M_PI) / preset->wavelength;
    float temporal_phase = time_sec * preset->speed * 2.0f * M_PI;
    float amplitude = preset->amplitude / 100.0f;

    // Scale amplitude with ramp progress (animated sunrise)
    if (amplitude_scale >= 0.0f) {
        if (amplitude_scale > 1.0f) amplitude_scale = 1.0f;
        amplitude *= amplitude_scale;
    }

    // Use override if provided, otherwise use preset value
    float base = (base_override >= 0)
        ? (base_override / 100.0f)
        : (preset->base_brightness / 100.0f);

    // Variation scales from 0-100% to 0-20% actual variation
    // During sunrise ramp, scale variation with the same curve as amplitude
    float variation_strength = preset->variation / 100.0f * 0.4f;
    if (amplitude_scale >= 0.0f) {
        variation_strength *= amplitude_scale;
    }

    bool use_hyperbolic = shape && (shape->kind == WAVE_SHAPE_HYPERBOLIC_AC || shape->kind == WAVE_SHAPE_HYPERBOLIC_PULSE);
    float hk = use_hyperbolic && shape->k > 0.01f ? shape->k : 0.01f;
    float hi = use_hyperbolic ? shape->i : 0.0f;
    float inv_sinh_k = use_hyperbolic ? 1.0f / sinhf(hk) : 0.0f;

    for (uint16_t i = 0; i < led_count; i++) {
        // Spatial phase based on LED position
        float spatial_phase = i * spatial_freq;
        float phase = spatial_phase + temporal_phase;

        float wave = resolve_wave(phase, shape, hk, hi, inv_sinh_k);

        // Organic variation using slow-moving noise
        float noise_x = i * 0.1f + time_sec * 0.3f;
        float variation = 1.0f + (value_noise(noise_x, 42.0f) - 0.5f) * 2.0f * variation_strength;

        // Combine: base + amplitude * wave * variation
        // Amplitude is halved because the wave is symmetric in [-1, 1]-ish
        // and we want symmetry around base
        float brightness = base + amplitude * 0.5f * wave * variation;

        // Clamp to valid range [0, 1]
        if (brightness < 0.0f) brightness = 0.0f;
        if (brightness > 1.0f) brightness = 1.0f;

        // Convert to percentage
        brightness_out[i] = (uint8_t)(brightness * 100.0f);
    }
}
