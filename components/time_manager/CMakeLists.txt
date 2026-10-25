idf_component_register(
    SRCS "time_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif
    PRIV_REQUIRES config_manager
)
