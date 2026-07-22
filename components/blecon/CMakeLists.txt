idf_component_register(
    SRCS "blecon.c" "ble_gatt.c"
    INCLUDE_DIRS "include"
    REQUIRES bt protocol
)
