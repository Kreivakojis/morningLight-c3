# MorningLight-Mini

A lighter, standalone derivative of [morningLight](../morningLight), targeting the
ESP32-C3 Super Mini module. Same core smart-light feature set (WiFi
AP/STA, web UI, NVS-backed config, animation/sunrise engine, MQTT/Home
Assistant discovery), but with a single addressable WS2811 strip only.

Dropped relative to the original:
- Second WS2811 strip and the discrete PWM RGB connector (single-strip
  output only — `led_driver_ws2811.c` is collapsed to one instance).
- Power monitoring (AC/DC current, DC voltage).
- Temperature sensing (NTC + dual DS18B20).

These were dropped because the ESP32-C3 doesn't have the hardware headroom
for them as originally implemented: it has only one general-purpose SPI
host (vs. classic ESP32's two, which the original dual-strip driver relied
on) and far fewer ADC channels/GPIOs overall.

## Building

```
idf.py set-target esp32c3
idf.py build
idf.py -p <port> flash monitor
```

GPIO defaults (see `main/Kconfig.projbuild`) assume a C3 Super Mini board;
adjust via `idf.py menuconfig` if your wiring differs. Avoid GPIO0/2/8/9
(strapping pins) and GPIO18-21 (USB/UART0) for new assignments.

## Known caveat: `led_strip` managed component needs a one-line patch

The `espressif/led_strip` package (v2.5.5, pulled automatically into
`managed_components/`, which is gitignored and gets redownloaded on a clean
build) has a pre-existing bug in its SPI backend: `src/led_strip_spi_dev.c`
uses `MALLOC_CAP_*`/`heap_caps_calloc` without including
`esp_heap_caps.h`. This happens to compile on classic ESP32 (something in
its header chain pulls it in transitively) but fails to compile for
ESP32-C3. This is an upstream issue, unrelated to anything in this repo's
own code.

If a clean build fails with `'MALLOC_CAP_DEFAULT' undeclared` in
`led_strip_spi_dev.c`, add this include near the top of that file (in
`managed_components/espressif__led_strip/src/led_strip_spi_dev.c`):

```c
#include "esp_heap_caps.h"
```

Check first whether a newer `espressif/led_strip` release has fixed this
upstream (the version pin is in `main/idf_component.yml`) before patching —
it may no longer be necessary.
