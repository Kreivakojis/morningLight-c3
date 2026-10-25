idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event
    PRIV_REQUIRES config_manager led_controller sunrise_engine animation_engine time_manager espressif__cjson
)
