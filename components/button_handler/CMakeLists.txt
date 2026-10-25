idf_component_register(
    SRCS "button_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio esp_event freertos esp_timer
)
