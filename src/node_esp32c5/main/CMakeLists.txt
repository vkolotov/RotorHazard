# RotorHazard's node core is compiled from ../../node unchanged; the C5
# supplies the receiver (RHNODE_RADIO_BACKEND) and the platform.
set(RH_NODE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../node)

# RHNODE_C5_APP=node (default): the RotorHazard node. =quadsim: the bench
# VTX emulator (c5_quadsim.cpp), which transmits.
if(NOT DEFINED RHNODE_C5_APP)
    set(RHNODE_C5_APP "node")
endif()
if(RHNODE_C5_APP STREQUAL "quadsim")
    set(APP_SRCS "c5_quadsim.cpp" "c5_radio.cpp" "c5_scan.cpp" "c5_meter.cpp")
else()
    set(APP_SRCS "c5_main.cpp" "c5_radio.cpp" "c5_scan.cpp" "c5_meter.cpp" "c5_rhglue.cpp"
         "${RH_NODE_DIR}/RssiNode.cpp" "${RH_NODE_DIR}/commands.cpp")
endif()

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "." "shim" "${RH_NODE_DIR}"
    PRIV_REQUIRES esp_wifi esp_phy esp_timer nvs_flash esp_driver_uart esp_driver_usb_serial_jtag esp_driver_gpio heap)

if(NOT DEFINED RHNODE_C5_BOARD)
    set(RHNODE_C5_BOARD "c5zero")
endif()
if(NOT DEFINED RHNODE_C5_LINK_TX)
    set(RHNODE_C5_LINK_TX -1)
endif()
if(NOT DEFINED RHNODE_C5_LINK_RX)
    set(RHNODE_C5_LINK_RX -1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RHNODE_ESP32C5=1
    RHNODE_C5_BOARD="${RHNODE_C5_BOARD}"
    RHNODE_C5_LINK_TX=${RHNODE_C5_LINK_TX}
    RHNODE_C5_LINK_RX=${RHNODE_C5_LINK_RX})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-volatile)
