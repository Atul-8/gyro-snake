# Board port: the only code that touches bsp_epd_*, NVS, LEDC and button edges.
idf_component_register(
    SRCS
        "src/display_port.c"
        "src/input_port.c"
        "src/audio_port.c"
        "src/storage_port.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bsp
        game_core
        nvs_flash
        esp_timer
        esp_driver_gpio
        esp_driver_ledc
        freertos
        log
)