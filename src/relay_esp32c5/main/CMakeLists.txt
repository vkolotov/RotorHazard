idf_component_register(
    SRCS "relay_main.cpp" "network.cpp" "core/web_form.cpp" "core/node_protocol.cpp" "core/node_driver.cpp" "core/race_engine.cpp"
         "core/channels.cpp" "core/mach1_timer.cpp" "core/feed_url.cpp" "core/usb_link.cpp" "firmware_update.cpp" "battery.cpp" "osd_backpack.cpp" "core/backpack.cpp"
    INCLUDE_DIRS "core"
    PRIV_REQUIRES app_update esp_adc esp_http_client esp_http_server esp_wifi esp_netif esp_event nvs_flash esp_driver_uart esp_timer json console
                  mbedtls esp_driver_usb_serial_jtag esp_driver_gpio)

# Default link pins (console `linkpins` overrides them in NVS). C5-Zero: its
# TX/RX pads, GPIO11/12. ESP32-S3 dev boards: GPIO15/16 since 0.3.6; 43/44
# (UART0, the COM port's USB-UART bridge) are kept free for pairing relays,
# and 17/18 are the OSD Timer Backpack's UART2.
if(IDF_TARGET STREQUAL "esp32s3")
    set(RELAY_DEFAULT_TX 15)
    set(RELAY_DEFAULT_RX 16)
else()
    set(RELAY_DEFAULT_TX 11)
    set(RELAY_DEFAULT_RX 12)
endif()
# The relay's battery divider (ADC1): GPIO1 on the S3 boards, none on the C5.
if(NOT DEFINED RELAY_VBAT_GPIO)
    if(IDF_TARGET STREQUAL "esp32s3")
        set(RELAY_VBAT_GPIO 1)
    else()
        set(RELAY_VBAT_GPIO -1)
    endif()
endif()
# The OSD Timer Backpack's UART (TX, RX): S3 UART2 on GPIO17/18; none on the C5.
if(NOT DEFINED RELAY_BACKPACK_TX)
    if(IDF_TARGET STREQUAL "esp32s3")
        set(RELAY_BACKPACK_TX 17)
        set(RELAY_BACKPACK_RX 18)
    else()
        set(RELAY_BACKPACK_TX -1)
        set(RELAY_BACKPACK_RX -1)
    endif()
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
    RELAY_LINK_TX=${RELAY_LINK_TX} RELAY_LINK_RX=${RELAY_LINK_RX} RELAY_VBAT_GPIO=${RELAY_VBAT_GPIO}
    RELAY_BACKPACK_TX=${RELAY_BACKPACK_TX} RELAY_BACKPACK_RX=${RELAY_BACKPACK_RX}
    RELAY_BOARD="${RELAY_BOARD}" RELAY_BOARD_UC="${RELAY_BOARD_UC}")
