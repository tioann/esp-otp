idf_component_register(
    SRCS "frame.c" "dispatch.c"
    INCLUDE_DIRS "include"
    REQUIRES store clocksrc totp display input esp_timer esp_system
)
