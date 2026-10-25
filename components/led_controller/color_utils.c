#include <math.h>
#include <stdint.h>

// PWM max value for 12-bit resolution
#define PWM_MAX 4095

/**
 * @brief Apply gamma correction to convert linear brightness to PWM duty
 *
 * @param value Linear brightness (0-255)
 * @param gamma Gamma value (typically 2.2)
 * @return PWM duty cycle (0-4095)
 */
uint32_t color_utils_gamma_correct(uint8_t value, float gamma)
{
    if (value == 0) return 0;
    if (value == 255) return PWM_MAX;

    float normalized = value / 255.0f;
    float corrected = powf(normalized, gamma);
    return (uint32_t)(corrected * PWM_MAX);
}

/**
 * @brief Convert HSV to RGB color space
 *
 * @param h Hue (0-360)
 * @param s Saturation (0-100)
 * @param v Value (0-100)
 * @param r Output red (0-255)
 * @param g Output green (0-255)
 * @param b Output blue (0-255)
 */
void color_utils_hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                            uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) {
        // Achromatic (gray)
        uint8_t val = (v * 255) / 100;
        *r = *g = *b = val;
        return;
    }

    // Normalize to 0-1 range
    float hf = (h % 360) / 60.0f;
    float sf = s / 100.0f;
    float vf = v / 100.0f;

    int i = (int)hf;
    float f = hf - i;
    float p = vf * (1.0f - sf);
    float q = vf * (1.0f - sf * f);
    float t = vf * (1.0f - sf * (1.0f - f));

    float rf, gf, bf;

    switch (i) {
        case 0:  rf = vf; gf = t;  bf = p;  break;
        case 1:  rf = q;  gf = vf; bf = p;  break;
        case 2:  rf = p;  gf = vf; bf = t;  break;
        case 3:  rf = p;  gf = q;  bf = vf; break;
        case 4:  rf = t;  gf = p;  bf = vf; break;
        default: rf = vf; gf = p;  bf = q;  break;
    }

    *r = (uint8_t)(rf * 255);
    *g = (uint8_t)(gf * 255);
    *b = (uint8_t)(bf * 255);
}

/**
 * @brief Convert color temperature (Kelvin) to RGB
 *
 * Based on Tanner Helland's algorithm for approximating blackbody radiation.
 *
 * @param kelvin Color temperature (1000-10000K)
 * @param r Output red (0-255)
 * @param g Output green (0-255)
 * @param b Output blue (0-255)
 */
void color_utils_kelvin_to_rgb(uint16_t kelvin, uint8_t *r, uint8_t *g, uint8_t *b)
{
    // Clamp to valid range
    if (kelvin < 1000) kelvin = 1000;
    if (kelvin > 10000) kelvin = 10000;

    float temp = kelvin / 100.0f;
    float red, green, blue;

    // Red
    if (temp <= 66) {
        red = 255;
    } else {
        red = 329.698727446f * powf(temp - 60, -0.1332047592f);
        if (red < 0) red = 0;
        if (red > 255) red = 255;
    }

    // Green
    if (temp <= 66) {
        green = 99.4708025861f * logf(temp) - 161.1195681661f;
    } else {
        green = 288.1221695283f * powf(temp - 60, -0.0755148492f);
    }
    if (green < 0) green = 0;
    if (green > 255) green = 255;

    // Blue
    if (temp >= 66) {
        blue = 255;
    } else if (temp <= 19) {
        blue = 0;
    } else {
        blue = 138.5177312231f * logf(temp - 10) - 305.0447927307f;
        if (blue < 0) blue = 0;
        if (blue > 255) blue = 255;
    }

    *r = (uint8_t)red;
    *g = (uint8_t)green;
    *b = (uint8_t)blue;
}

/**
 * @brief Linear interpolation between two RGB colors
 *
 * @param r1, g1, b1 Start color
 * @param r2, g2, b2 End color
 * @param t Interpolation factor (0.0 to 1.0)
 * @param r, g, b Output color
 */
void color_utils_lerp_rgb(uint8_t r1, uint8_t g1, uint8_t b1,
                          uint8_t r2, uint8_t g2, uint8_t b2,
                          float t, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    *r = (uint8_t)(r1 + t * (r2 - r1));
    *g = (uint8_t)(g1 + t * (g2 - g1));
    *b = (uint8_t)(b1 + t * (b2 - b1));
}

/**
 * @brief Calculate perceived brightness of RGB color
 *
 * Uses ITU-R BT.709 coefficients for luminance.
 *
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 * @return Perceived brightness (0-255)
 */
uint8_t color_utils_perceived_brightness(uint8_t r, uint8_t g, uint8_t b)
{
    // Luminance coefficients: R=0.2126, G=0.7152, B=0.0722
    float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return (uint8_t)luminance;
}
