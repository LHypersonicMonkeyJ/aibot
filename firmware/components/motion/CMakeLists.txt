idf_component_register(
    SRCS "i2c_bus.cpp" "pca9685.cpp" "servo_driver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
