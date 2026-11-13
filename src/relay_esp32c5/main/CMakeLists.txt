idf_component_register(
    SRCS "relay_main.cpp" "network.cpp" "core/web_form.cpp" "core/node_protocol.cpp" "core/node_driver.cpp" "core/race_engine.cpp"
         "core/channels.cpp" "core/mach1_timer.cpp" "core/feed_url.cpp"
    INCLUDE_DIRS "core"
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash esp_driver_uart esp_timer json console
                  mbedtls esp_driver_usb_serial_jtag)

# Default link pins: the board's TX/RX pads (UART0's pins), which the ROM
# also prints its boot banner on. C5-Zero: GPIO11/12. ESP32-S3 dev boards:
# GPIO43/44, shared with the COM port's USB-UART bridge.
if(IDF_TARGET STREQUAL "esp32s3")
    set(RELAY_DEFAULT_TX 43)
    set(RELAY_DEFAULT_RX 44)
else()
    set(RELAY_DEFAULT_TX 11)
    set(RELAY_DEFAULT_RX 12)
endif()
if(NOT DEFINED RELAY_LINK_TX)
    set(RELAY_LINK_TX ${RELAY_DEFAULT_TX})
endif()
if(NOT DEFINED RELAY_LINK_RX)
    set(RELAY_LINK_RX ${RELAY_DEFAULT_RX})
endif()
# The board's family in names (hello's rh_version, AP SSID, hostname): c5, s3.
string(REPLACE "esp32" "" RELAY_BOARD "${IDF_TARGET}")
string(TOUPPER "${RELAY_BOARD}" RELAY_BOARD_UC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RELAY_LINK_TX=${RELAY_LINK_TX} RELAY_LINK_RX=${RELAY_LINK_RX}
    RELAY_BOARD="${RELAY_BOARD}" RELAY_BOARD_UC="${RELAY_BOARD_UC}")
