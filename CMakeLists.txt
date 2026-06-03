# Carviston — Ariston VLS Evo 100 replacement controller
# Target: ESP32-S3-N16R8

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(carviston)

# The Vue frontend is built and embedded directly into the firmware image by
# the `main` component (see main/CMakeLists.txt) — there is no SPIFFS web
# partition any more.
