idf_component_register(
    SRCS "lcd.cpp" "face.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio
)
# Rendering runs every frame, so optimize it even in debug builds.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
