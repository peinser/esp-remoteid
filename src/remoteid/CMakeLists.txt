idf_component_register(
    SRCS
        "main.c"
        "ota/ota.c"
        "remoteid/auth.c"
        "remoteid/ble.c"
        "remoteid/encode.c"
        "led/indicator.c"
        "led/lighting.c"
        "remoteid/mavlink.c"
        "remoteid/model.c"
        "led/pattern.c"
        "remoteid/store.c"
        "remoteid/wifi.c"
    INCLUDE_DIRS
        "."
        "led"
        "ota"
        "remoteid"
    REQUIRES
        app_update
        bt
        json
        esp_driver_gpio
        esp_driver_rmt
        esp_driver_uart
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        mbedtls
        monocypher
        nvs_flash
        opendroneid_core
)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/mavlink_c_library_v2"
)

# Write a header that produces a compile-time #error when Release mode is active
# without the explicit confirmation. Using a generated header instead of
# message(FATAL_ERROR) means configure (and therefore menuconfig) always
# succeeds; only the actual build is blocked.
if(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE AND
   NOT "${CONFIG_REMOTEID_FLASH_ENC_RELEASE_CONFIRM}" STREQUAL "UNRECOVERABLE")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/flash_enc_guard.h"
        "#error \"Flash encryption Release mode is enabled but the build guard is not satisfied. "
        "Release mode permanently burns eFuses on first boot and disables serial flashing and JTAG. "
        "Set: ESP Remote ID -> Flash encryption Release mode confirmation = UNRECOVERABLE\"\n")
else()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/flash_enc_guard.h" "/* flash encryption guard satisfied */\n")
endif()

target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
