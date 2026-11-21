idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_port
        imu_port
        bsp
        game_core
        imu_core
        nvs_flash
        esp_driver_gpio
        heap
        freertos
        log
)