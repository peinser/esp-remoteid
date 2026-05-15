idf_component_register(
    SRCS
        "main.c"
        "remoteid/auth.c"
        "remoteid/ble.c"
        "remoteid/encode.c"
        "remoteid/indicator.c"
        "remoteid/lighting.c"
        "remoteid/mavlink.c"
        "remoteid/model.c"
        "remoteid/pattern.c"
        "remoteid/store.c"
        "remoteid/wifi.c"
    INCLUDE_DIRS
        "."
        "remoteid"
    REQUIRES
        bt
        esp_driver_gpio
        esp_driver_rmt
        esp_driver_uart
        esp_event
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

if(CONFIG_FLASH_ENCRYPTION_MODE_RELEASE AND
   NOT "${CONFIG_REMOTEID_FLASH_ENC_RELEASE_CONFIRM}" STREQUAL "UNRECOVERABLE")
    message(FATAL_ERROR
        "\n"
        "  Flash encryption Release mode is enabled but the build guard is not satisfied.\n"
        "  Release mode permanently burns eFuses on first boot that disable serial\n"
        "  flashing and JTAG. This cannot be undone without replacing the SoC.\n"
        "\n"
        "  To confirm you understand this, set:\n"
        "    ESP Remote ID → Flash encryption Release mode confirmation = UNRECOVERABLE\n"
    )
endif()
