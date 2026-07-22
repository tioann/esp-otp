# Pull the components' Unity test sources into the test runner.
idf_component_register(
    SRCS
        "test_main.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../components/totp/test/test_totp.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../components/protocol/test/test_protocol.c"
        "${CMAKE_CURRENT_LIST_DIR}/../../components/store/test/test_store.c"
    INCLUDE_DIRS "."
    REQUIRES unity totp protocol store nvs_flash
    WHOLE_ARCHIVE
)
