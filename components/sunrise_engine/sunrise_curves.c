#include <math.h>
#include "sunrise_engine.h"

/**
 * @brief Apply brightness curve to linear progress
 *
 * @param curve Curve type
 * @param t Linear progress (0.0 to 1.0)
 * @return Curved progress (0.0 to 1.0)
 */
float sunrise_curve_apply(sunrise_curve_t curve, float t)
{
    // Clamp input
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    switch (curve) {
        case SUNRISE_CURVE_LINEAR:
            // y = t
            return t;

        case SUNRISE_CURVE_LOGARITHMIC:
            // y = log10(1 + 9*t)
            // This gives a natural-feeling brightness ramp
            // Starts slow, accelerates in the middle, slows at the end
            return log10f(1.0f + 9.0f * t);

        case SUNRISE_CURVE_SIGMOID:
            // y = 1 / (1 + e^(-12*(t-0.5)))
            // S-curve: slow start, fast middle, slow end
            return 1.0f / (1.0f + expf(-12.0f * (t - 0.5f)));

        case SUNRISE_CURVE_EXPONENTIAL:
            // y = (e^(3t) - 1) / (e^3 - 1)
            // Starts very slow, accelerates rapidly at the end
            {
                float e3 = expf(3.0f);
                return (expf(3.0f * t) - 1.0f) / (e3 - 1.0f);
            }

        case SUNRISE_CURVE_INVERSE_LOG:
            // y = 1 - log10(10 - 9*t)
            // Fast initial ramp, slows at end (inverse of logarithmic)
            return 1.0f - log10f(10.0f - 9.0f * t);

        default:
            return t;
    }
}

/**
 * @brief Get curve name string
 *
 * @param curve Curve type
 * @return Curve name
 */
const char *sunrise_curve_get_name(sunrise_curve_t curve)
{
    switch (curve) {
        case SUNRISE_CURVE_LINEAR:      return "linear";
        case SUNRISE_CURVE_LOGARITHMIC: return "logarithmic";
        case SUNRISE_CURVE_SIGMOID:     return "sigmoid";
        case SUNRISE_CURVE_EXPONENTIAL: return "exponential";
        case SUNRISE_CURVE_INVERSE_LOG: return "inverse logarithmic";
        default:                        return "unknown";
    }
}

/**
 * @brief Interpolate color temperature during sunrise
 *
 * Simulates natural sunrise: starts warm (red/orange), ends cooler (warm white).
 *
 * @param progress Sunrise progress (0.0 to 1.0)
 * @param start_temp Starting temperature (e.g., 1800K for deep red)
 * @param end_temp Ending temperature (e.g., 4000K for warm white)
 * @return Interpolated color temperature
 */
uint16_t sunrise_color_interpolate(float progress, uint16_t start_temp, uint16_t end_temp)
{
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    // Use a slight curve for color transition (slower at start)
    float color_progress = powf(progress, 0.7f);

    return start_temp + (uint16_t)(color_progress * (end_temp - start_temp));
}
