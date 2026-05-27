# Carviston — Ariston VLS Evo 100 replacement controller
# Target: ESP32-S3-N16R8

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(carviston)

# --- Web frontend → SPIFFS image -------------------------------------------
# Always invoke `npm run build` (Vite has its own incremental cache so this is
# near-free when nothing changed). Output goes to build/web_image, which the
# spiffs partition image generator picks up.

set(WEB_SRC_DIR     ${CMAKE_SOURCE_DIR}/web)
set(WEB_IMAGE_DIR   ${CMAKE_BINARY_DIR}/web_image)

file(MAKE_DIRECTORY ${WEB_IMAGE_DIR})

add_custom_target(web_bundle ALL
    COMMAND bash ${WEB_SRC_DIR}/build.sh ${WEB_IMAGE_DIR}
    WORKING_DIRECTORY ${WEB_SRC_DIR}
    COMMENT "Building Vue frontend → SPIFFS"
    USES_TERMINAL
    VERBATIM)

spiffs_create_partition_image(web ${WEB_IMAGE_DIR}
    FLASH_IN_PROJECT
    DEPENDS web_bundle)
