# IMU port: LSM6DS3TR-C over the ESP-IDF i2c_master driver (I2C1).
idf_component_register(
    SRCS "src/imu_port.c"
    INCLUDE_DIRS "include"
    REQUIRES
        board_port
        imu_core
        esp_driver_i2c
        freertos
        log
)