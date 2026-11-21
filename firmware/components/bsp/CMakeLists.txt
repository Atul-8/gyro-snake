idf_component_register(
    SRCS "src/bsp_epd.c" "src/bsp_button.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_driver_gpio esp_driver_spi esp_timer freertos log
)
