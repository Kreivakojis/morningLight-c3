# MorningLight-Mini - ESP32-C3 Sunrise Alarm Clock (single-strip derivative of morningLight)
# Root CMakeLists.txt

cmake_minimum_required(VERSION 3.16)

# Set extra component directories before project()
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(morninglight_mini)
