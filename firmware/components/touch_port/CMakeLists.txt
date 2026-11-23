idf_component_register(
    SRCS "src/touch_port.c"
    INCLUDE_DIRS "include"
    REQUIRES bsp esp_driver_i2c esp_driver_gpio esp_timer freertos log
)