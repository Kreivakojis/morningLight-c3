idf_component_register(
    SRCS
        "animation_engine.c"
        "wave_generator.c"
    INCLUDE_DIRS "include"
    REQUIRES led_controller config_manager freertos
)
