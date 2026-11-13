# RotorHazard relay for the ESP32-C5 or ESP32-S3 (ESP-IDF 5.5): drives a
# RotorHazard node over a UART (the AirPort radio link, or a wire) and is a
# mach1 timer.
#   idf.py -B build set-target esp32c5 && idf.py -B build build
#   idf.py -B build-s3 set-target esp32s3 && idf.py -B build-s3 build
# Link pins (default 11/12: the C5-Zero's TX/RX pads, free GPIOs on an S3):
#   -DRELAY_LINK_TX=<gpio> -DRELAY_LINK_RX=<gpio>
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rhrelay_esp32c5)
