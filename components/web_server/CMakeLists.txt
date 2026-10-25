idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_event
    PRIV_REQUIRES wifi_manager config_manager time_manager led_controller sunrise_engine animation_engine dns_server mqtt_manager
    EMBED_FILES "embedded/index.html" "embedded/style.css" "embedded/app.js"
)
