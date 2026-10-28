idf_component_register(
    SRCS "main.cpp" "servo_commands.cpp" "display_commands.cpp" "audio_commands.cpp"
    INCLUDE_DIRS "."
    REQUIRES communication motion display audio esp_timer
)
