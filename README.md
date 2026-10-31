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

**Use a stable ESP-IDF release (v5.4 confirmed working), not `master`.**
Building this project against an ESP-IDF `master`/dev snapshot produced a
softAP that started without errors in the logs but never actually
broadcast (no beacon/probe/auth traffic reached a client, even at point-
blank range) — a WiFi regression in that particular nightly's ESP32-C3
path, not a hardware or antenna problem, not a config/code problem on this
project's side. Re-building the exact same source against stable
ESP-IDF v5.4 fixed it immediately — AP visible with good signal strength,
clients connect and get a DHCP lease normally. Confirmed on real C3 Super
Mini hardware.

```
idf.py set-target esp32c3
idf.py build
idf.py -p <port> flash monitor
```

GPIO defaults (see `main/Kconfig.projbuild`) assume a C3 Super Mini board;
adjust via `idf.py menuconfig` if your wiring differs. Avoid GPIO0/2/8/9
(strapping pins) and GPIO18-21 (USB/UART0) for new assignments.

Console output goes over the native USB-Serial-JTAG peripheral (the same
port used for flashing — `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in
`sdkconfig.defaults`), since the C3 Super Mini has no separate USB-UART
bridge chip exposing UART0.

## Note on the `led_strip` managed component

`espressif/led_strip` v2.5.5 (pulled automatically into
`managed_components/`, gitignored, redownloaded on a clean build) has
`src/led_strip_spi_dev.c` use `MALLOC_CAP_*`/`heap_caps_calloc` without
directly including `esp_heap_caps.h`. This was first seen as a hard build
failure (`'MALLOC_CAP_DEFAULT' undeclared`), but turned out to be entangled
with the broken IDF master snapshot above, not a standalone upstream bug:
building against stable ESP-IDF v5.4, the same unpatched vendored file
compiles cleanly (v5.4's header chain pulls in `esp_heap_caps.h`
transitively where master's didn't). No patch needed on v5.4. If a clean
build ever fails with that exact error again, add
`#include "esp_heap_caps.h"` near the top of that file as a workaround,
but treat it as a sign something's off with the IDF version in use first.
