idf_component_register(
    SRCS "relay_main.cpp" "network.cpp" "core/web_form.cpp" "core/node_protocol.cpp" "core/node_driver.cpp" "core/race_engine.cpp"
         "core/channels.cpp" "core/mach1_timer.cpp" "core/feed_url.cpp"
    INCLUDE_DIRS "core"
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash esp_driver_uart esp_timer json console
                  mbedtls esp_driver_usb_serial_jtag)

if(NOT DEFINED RELAY_LINK_TX)
    set(RELAY_LINK_TX 11)
endif()
if(NOT DEFINED RELAY_LINK_RX)
    set(RELAY_LINK_RX 12)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RELAY_LINK_TX=${RELAY_LINK_TX} RELAY_LINK_RX=${RELAY_LINK_RX})
