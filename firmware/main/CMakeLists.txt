idf_component_register(
    SRCS "main.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_port
        imu_port
        touch_port
        bsp
        game_core
        imu_core
        nvs_flash
        esp_driver_gpio
        esp_driver_uart
        heap
        freertos
        log
)