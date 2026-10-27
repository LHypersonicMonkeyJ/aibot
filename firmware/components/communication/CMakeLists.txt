idf_component_register(
    SRCS "serial_protocol.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_uart
)
