#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_driver.h"
#include "config_manager.h"

static const char *TAG = "led_ws2811";

// Max supported LEDs on the strip
#define MAX_LED_COUNT       300

static float get_gamma(void) {
    device_config_t *cfg = config_manager_get();
    uint8_t g = cfg ? cfg->gamma_x10 : 0;
    return (g >= 10 && g <= 40) ? g / 10.0f : 2.2f;
}

typedef struct {
    bool enabled;
    uint8_t freq_hz;            // How often the error-diffusion step updates
    uint8_t max_brightness_pct; // Dithering only applies at or below this brightness
} dither_settings_t;

static dither_settings_t get_dither_settings(void) {
    device_config_t *cfg = config_manager_get();
    if (!cfg) {
        return (dither_settings_t){ .enabled = false, .freq_hz = 10, .max_brightness_pct = 100 };
    }
    uint8_t freq = cfg->dither_freq_hz;
    if (freq < 1) freq = 1;
    if (freq > 60) freq = 60;
    return (dither_settings_t){
        .enabled = cfg->dither_enabled,
        .freq_hz = freq,
        .max_brightness_pct = cfg->dither_max_brightness_pct,
    };
}

static struct {
    led_strip_handle_t strip;
    uint16_t led_count;
    uint8_t r, g, b;
    uint8_t brightness;
    bool initialized;      // led_strip/SPI device created (only if config-enabled)
    gpio_num_t enable_gpio;
    bool enable_gpio_valid;
    int enable_level;      // last level written to the enable GPIO (-1 = unknown)

    // Temporal dithering error accumulators (one slot per LED per channel).
    // Carries the rounding error from each 8-bit quantization into the next
    // dither step's target so consecutive steps alternate between adjacent
    // output levels in a ratio that averages to the true fractional
    // brightness, instead of visibly snapping between them. See
    // set_pixel_brightnesses_impl().
    float dither_err_r[MAX_LED_COUNT];
    float dither_err_g[MAX_LED_COUNT];
    float dither_err_b[MAX_LED_COUNT];

    // Last dithered output held between update steps when the configured
    // dither frequency is slower than the animation frame rate, and the
    // wall-clock time of that last step.
    uint8_t dither_hold_r[MAX_LED_COUNT];
    uint8_t dither_hold_g[MAX_LED_COUNT];
    uint8_t dither_hold_b[MAX_LED_COUNT];
    int64_t last_dither_step_us;
} ws2811_state = {0};

static bool array_has_nonzero(const uint8_t *arr, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        if (arr[i] > 0) {
            return true;
        }
    }
    return false;
}

// Quantizes a float target (0-255) to uint8_t, carrying the leftover
// fractional error forward in *err_acc so it's added into next frame's
// target. Over several frames the output's time-average converges on the
// true target even when the target itself only ever rounds to one of two
// adjacent integers each frame.
static uint8_t dither_quantize(float target, float *err_acc)
{
    float value = target + *err_acc;
    if (value < 0.0f) value = 0.0f;
    if (value > 255.0f) value = 255.0f;

    uint8_t out = (uint8_t)(value + 0.5f);
    *err_acc = value - out;
    return out;
}

// Power-gates the strip's ground return via its low-side MOSFET. Only
// writes the GPIO on a transition to avoid redundant writes every
// animation frame (~30fps).
static void set_enable(bool on)
{
    if (!ws2811_state.enable_gpio_valid) {
        return;
    }

    int level = on ? 1 : 0;
    if (ws2811_state.enable_level == level) {
        return;
    }

    gpio_set_level(ws2811_state.enable_gpio, level);
    ws2811_state.enable_level = level;
}

static void update_pixels_impl(void)
{
    if (!ws2811_state.initialized || !ws2811_state.strip) {
        return;
    }

    float brightness_scale = ws2811_state.brightness / 100.0f;

    // Gamma-correct the color at full intensity, then scale by brightness.
    // This preserves low-brightness resolution. The old approach of scaling
    // first then gamma-correcting crushed small values to zero through the
    // 12-bit PWM intermediary (e.g. 8% brightness → output 0).
    float gamma = get_gamma();
    uint8_t out_r = (uint8_t)(powf(ws2811_state.r / 255.0f, gamma) * brightness_scale * 255.0f + 0.5f);
    uint8_t out_g = (uint8_t)(powf(ws2811_state.g / 255.0f, gamma) * brightness_scale * 255.0f + 0.5f);
    uint8_t out_b = (uint8_t)(powf(ws2811_state.b / 255.0f, gamma) * brightness_scale * 255.0f + 0.5f);

    // Set all LEDs to the same color
    // WS2811 color order correction: swap G and B
    for (uint16_t i = 0; i < ws2811_state.led_count; i++) {
        led_strip_set_pixel(ws2811_state.strip, i, out_r, out_b, out_g);
    }

    esp_err_t ret = led_strip_refresh(ws2811_state.strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip refresh failed: %s", esp_err_to_name(ret));
    }

    set_enable(out_r > 0 || out_g > 0 || out_b > 0);
}

static esp_err_t init_impl(gpio_num_t data_gpio, gpio_num_t enable_gpio,
                           bool cfg_enabled, uint16_t led_count)
{
    // Configure the enable GPIO as output, driven LOW immediately — fail-safe
    // dark, even if the strip ends up config-disabled (defends against a
    // floating gate if the connector is physically present but unused).
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << enable_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure enable GPIO %d: %s", enable_gpio, esp_err_to_name(ret));
    }
    ws2811_state.enable_gpio = enable_gpio;
    ws2811_state.enable_gpio_valid = (ret == ESP_OK);
    ws2811_state.enable_level = -1;
    set_enable(false);

    if (!cfg_enabled) {
        ESP_LOGI(TAG, "Addressable strip disabled in config, skipping strip init");
        return ESP_OK;
    }

    if (led_count == 0) led_count = 30;
    if (led_count > MAX_LED_COUNT) led_count = MAX_LED_COUNT;
    ws2811_state.led_count = led_count;

    ESP_LOGI(TAG, "Configuring LED strip: %d LEDs on GPIO %d (enable GPIO %d)",
             led_count, data_gpio, enable_gpio);

    led_strip_config_t strip_config = {
        .strip_gpio_num = data_gpio,
        .max_leds = led_count,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    // SPI+DMA backend instead of RMT: on the original classic-ESP32 board
    // this component was written for, RMT has no DMA path and only 64 words
    // of hardware buffer per channel, so a long WS2811 frame depends on a
    // timely interrupt-driven refill — which WiFi's driver (its critical
    // sections mask all software interrupt priorities 1-3) was starving
    // badly enough to corrupt individual pixels. SPI+DMA transmits the whole
    // frame via a hardware-walked DMA descriptor once started, with no
    // per-chunk CPU intervention required mid-transfer, so a WiFi stall can
    // only delay when we learn the transfer finished, not corrupt bits
    // already in flight. This reasoning is inherited from the classic-ESP32
    // port rather than independently re-verified against the C3's WiFi/RMT
    // silicon, but keeping the same SPI+DMA approach here is lower-risk than
    // re-deriving an RMT-based implementation from scratch. ESP32-C3 has
    // exactly one general-purpose SPI/DMA host available to applications
    // (SPI1_HOST is reserved for flash; there is no SPI3_HOST on this chip,
    // unlike classic ESP32's two-host split) — SPI2_HOST is it.
    led_strip_spi_config_t spi_config = {
        .clk_src = SPI_CLK_SRC_DEFAULT,
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };

    ret = led_strip_new_spi_device(&strip_config, &spi_config, &ws2811_state.strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create LED strip: %s", esp_err_to_name(ret));
        return ret;
    }

    ws2811_state.initialized = true;
    ws2811_state.brightness = 0;
    ws2811_state.r = ws2811_state.g = ws2811_state.b = 0;
    memset(ws2811_state.dither_err_r, 0, sizeof(ws2811_state.dither_err_r));
    memset(ws2811_state.dither_err_g, 0, sizeof(ws2811_state.dither_err_g));
    memset(ws2811_state.dither_err_b, 0, sizeof(ws2811_state.dither_err_b));
    memset(ws2811_state.dither_hold_r, 0, sizeof(ws2811_state.dither_hold_r));
    memset(ws2811_state.dither_hold_g, 0, sizeof(ws2811_state.dither_hold_g));
    memset(ws2811_state.dither_hold_b, 0, sizeof(ws2811_state.dither_hold_b));
    ws2811_state.last_dither_step_us = 0;

    led_strip_clear(ws2811_state.strip);

    ESP_LOGI(TAG, "LED strip initialized: %d LEDs on GPIO %d", led_count, data_gpio);
    return ESP_OK;
}

static void set_rgb_impl(uint8_t r, uint8_t g, uint8_t b)
{
    if (!ws2811_state.initialized) return;

    // Just store values - don't refresh here
    // set_brightness() will trigger the actual refresh
    ws2811_state.r = r;
    ws2811_state.g = g;
    ws2811_state.b = b;
}

static void set_brightness_impl(uint8_t brightness)
{
    if (!ws2811_state.initialized) return;

    if (brightness > 100) brightness = 100;
    ws2811_state.brightness = brightness;
    update_pixels_impl();
}

static uint8_t get_brightness_impl(void)
{
    return ws2811_state.brightness;
}

static void set_pixel_brightnesses_impl(const uint8_t *brightness_array, uint16_t count)
{
    if (!ws2811_state.initialized || !ws2811_state.strip) {
        return;
    }
    if (brightness_array == NULL || count == 0) {
        return;
    }
    if (count > ws2811_state.led_count) {
        count = ws2811_state.led_count;
    }

    // Gamma-correct base color once at full intensity, then scale per-LED
    float gamma = get_gamma();
    float gamma_r = powf(ws2811_state.r / 255.0f, gamma) * 255.0f;
    float gamma_g = powf(ws2811_state.g / 255.0f, gamma) * 255.0f;
    float gamma_b = powf(ws2811_state.b / 255.0f, gamma) * 255.0f;
    dither_settings_t dset = get_dither_settings();

    // Gate how often the error-diffusion step actually advances. Holding the
    // last dithered value between steps (rather than re-running the
    // diffusion every animation frame, ~30Hz) lets the toggle rate between
    // adjacent 8-bit levels be slowed down independently of the animation's
    // own frame rate.
    int64_t now_us = esp_timer_get_time();
    int64_t period_us = 1000000LL / dset.freq_hz;
    bool dither_step = (now_us - ws2811_state.last_dither_step_us) >= period_us;
    if (dither_step) {
        ws2811_state.last_dither_step_us = now_us;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint8_t brightness = brightness_array[i];
        if (brightness > 100) brightness = 100;

        float scale = brightness / 100.0f;
        uint8_t out_r, out_g, out_b;

        // Dithering is only useful at low output levels, where an 8-bit
        // step is a large fraction of the signal — confine it to LEDs at or
        // below the configured brightness ceiling, plain-rounding the rest.
        if (dset.enabled && brightness <= dset.max_brightness_pct) {
            if (dither_step) {
                out_r = dither_quantize(gamma_r * scale, &ws2811_state.dither_err_r[i]);
                out_g = dither_quantize(gamma_g * scale, &ws2811_state.dither_err_g[i]);
                out_b = dither_quantize(gamma_b * scale, &ws2811_state.dither_err_b[i]);
                ws2811_state.dither_hold_r[i] = out_r;
                ws2811_state.dither_hold_g[i] = out_g;
                ws2811_state.dither_hold_b[i] = out_b;
            } else {
                out_r = ws2811_state.dither_hold_r[i];
                out_g = ws2811_state.dither_hold_g[i];
                out_b = ws2811_state.dither_hold_b[i];
            }
        } else {
            out_r = (uint8_t)(gamma_r * scale + 0.5f);
            out_g = (uint8_t)(gamma_g * scale + 0.5f);
            out_b = (uint8_t)(gamma_b * scale + 0.5f);
            // Reset so a later re-entry into the dithered brightness range
            // starts clean instead of carrying stale error/hold state.
            ws2811_state.dither_err_r[i] = 0.0f;
            ws2811_state.dither_err_g[i] = 0.0f;
            ws2811_state.dither_err_b[i] = 0.0f;
        }

        // WS2811 color order correction: swap G and B
        led_strip_set_pixel(ws2811_state.strip, i, out_r, out_b, out_g);
    }

    // Fill remaining LEDs with zero if count < led_count
    for (uint16_t i = count; i < ws2811_state.led_count; i++) {
        led_strip_set_pixel(ws2811_state.strip, i, 0, 0, 0);
        ws2811_state.dither_err_r[i] = 0.0f;
        ws2811_state.dither_err_g[i] = 0.0f;
        ws2811_state.dither_err_b[i] = 0.0f;
        ws2811_state.dither_hold_r[i] = 0;
        ws2811_state.dither_hold_g[i] = 0;
        ws2811_state.dither_hold_b[i] = 0;
    }

    led_strip_refresh(ws2811_state.strip);

    // Latch the enable gate ON the moment this animation/wave path renders any
    // nonzero pixel; never latch it back OFF from here. Per-frame wave/noise
    // content legitimately drives every LED to 0 at the trough of a low
    // base-brightness cycle, and re-deriving the enable GPIO from that per
    // frame power-cycled the strip's ground return in sync with the wave —
    // visible as flicker. The WS2812 protocol already renders (0,0,0) as
    // black with the gate held high, so there's no need to also cut power
    // mid-animation; only the explicit off path (set_brightness(0), via
    // update_pixels_impl) should de-assert it.
    if (array_has_nonzero(brightness_array, count) && (gamma_r > 0 || gamma_g > 0 || gamma_b > 0)) {
        set_enable(true);
    }
}

// ---------------------------------------------------------------------
// Driver trampolines
// ---------------------------------------------------------------------

static esp_err_t ws2811_init(void)
{
    device_config_t *cfg = config_manager_get();
    bool enabled = cfg ? cfg->ws1_enabled : true;
    uint16_t count = cfg ? cfg->ws1_led_count : 30;
    return init_impl(CONFIG_ML_GPIO_WS1_DATA, CONFIG_ML_GPIO_WS1_ENABLE, enabled, count);
}

static void ws2811_set_rgb(uint8_t r, uint8_t g, uint8_t b) { set_rgb_impl(r, g, b); }
static void ws2811_set_brightness(uint8_t brightness) { set_brightness_impl(brightness); }
static uint8_t ws2811_get_brightness(void) { return get_brightness_impl(); }
static void ws2811_set_pixel_brightnesses(const uint8_t *arr, uint16_t count) { set_pixel_brightnesses_impl(arr, count); }

// Export WS2811 driver operations
const led_driver_ops_t led_driver_ws2811 = {
    .init = ws2811_init,
    .set_rgb = ws2811_set_rgb,
    .set_brightness = ws2811_set_brightness,
    .get_brightness = ws2811_get_brightness,
    .set_pixel_brightnesses = ws2811_set_pixel_brightnesses,
};
