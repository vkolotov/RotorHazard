# RotorHazard relay for the ESP32-C5 (ESP-IDF 5.5): drives a RotorHazard node
# over a UART (the AirPort radio link, or a wire) and is a mach1 timer.
#   idf.py set-target esp32c5 && idf.py build
# Link pins (default 11/12, the C5-Zero's TX/RX pads):
#   -DRELAY_LINK_TX=<gpio> -DRELAY_LINK_RX=<gpio>
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rhrelay_esp32c5)
