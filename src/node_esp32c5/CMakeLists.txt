# RotorHazard node firmware for the ESP32-C5 (ESP-IDF 5.5).
#   idf.py set-target esp32c5 && idf.py build
# Board: -DRHNODE_C5_BOARD=c5zero (Waveshare ESP32-C5-Zero, default) or xiaoc5
# (Seeed XIAO ESP32-C5). A UART link for RS422 or a serial radio:
#   -DRHNODE_C5_LINK_TX=<gpio> -DRHNODE_C5_LINK_RX=<gpio>
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rhnode_esp32c5)
