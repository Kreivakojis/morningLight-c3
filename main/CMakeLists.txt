idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        led_controller
        status_led
        wifi_manager
        dns_server
        web_server
        config_manager
        time_manager
        sunrise_engine
        animation_engine
        button_handler
        mqtt_manager
        esp_event
        nvs_flash
)
