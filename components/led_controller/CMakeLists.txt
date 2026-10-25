idf_component_register(
    SRCS
        "led_controller.c"
        "led_driver_ws2811.c"
        "color_utils.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio esp_event config_manager
    PRIV_REQUIRES espressif__led_strip esp_timer
)
