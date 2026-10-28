idf_component_register(
    SRCS "main.cpp" "servo_commands.cpp"
    INCLUDE_DIRS "."
    REQUIRES communication motion esp_timer
)
