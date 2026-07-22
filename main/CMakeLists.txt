idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer nvs_flash display input clocksrc store totp usbcon blecon led
)
