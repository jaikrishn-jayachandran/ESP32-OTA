idf_component_register(
    SRCS
        "main.c"
        "firmware/nvs_manager.c"
        "firmware/wifi_manager.c"
        "firmware/mqtt_listener.c"
        "firmware/ota_engine.c"
        "firmware/status_led.c"
        "user_space/user_space.c"

    INCLUDE_DIRS
        "."
        "firmware"
        "user_space"

    
)