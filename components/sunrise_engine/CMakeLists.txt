idf_component_register(
    SRCS "sunrise_engine.c" "sunrise_curves.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event freertos
    PRIV_REQUIRES config_manager time_manager led_controller
)
