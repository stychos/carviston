idf_component_register(
    SRCS
        "main.c"
        "app_config.c"
        "temperature.c"
        "leds.c"
        "buttons.c"
        "relays.c"
        "safety.c"
        "heater_control.c"
        "auth.c"
        "wifi_mgr.c"
        "web_assets.c"
        "web_server.c"
        "ota.c"
        "event_log.c"
        "benchmark.c"
        "energy.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_driver_i2c
        esp_app_format
        app_update
        esp_driver_gpio
        esp_timer
        mbedtls
)

# --- Embedded web UI (replaces the old SPIFFS 'web' partition) --------------
# Build the Vue app (Vite's own incremental cache makes the no-change case
# ~1 s) and pack every built file into one archive, then link that archive
# straight into the firmware. Because the UI lives inside the app image, a
# firmware OTA always carries its matching UI — there is no separate web
# partition to keep in sync, mount, or flash.
set(WEB_SRC_DIR   ${CMAKE_SOURCE_DIR}/web)
set(WEB_IMAGE_DIR ${CMAKE_BINARY_DIR}/web_image)
set(WEB_BLOB      ${CMAKE_BINARY_DIR}/web_assets.bin)
set(WEB_GEN_DIR   ${CMAKE_BINARY_DIR}/gen)

# --- Project version (single source of truth) -------------------------------
# tools/gen-version.mjs computes 0.<CFG_VERSION>.<build>, bumps the per-build
# counter (web/.build_version), and writes both a C header the firmware
# compiles in (app_version.h → CARVISTON_VERSION) and a plain string the web
# build reads. An ALL target so it runs — and the build number advances — once
# per build, exactly like the web build. Existed before only in the footer; now
# the firmware reports the same string (see web_server.c).
file(MAKE_DIRECTORY ${WEB_GEN_DIR})   # ensure the -I dir exists at configure
add_custom_target(carviston_version ALL
    BYPRODUCTS ${WEB_GEN_DIR}/app_version.h ${WEB_GEN_DIR}/app_version.txt
    COMMAND node ${CMAKE_SOURCE_DIR}/tools/gen-version.mjs
            ${CMAKE_CURRENT_SOURCE_DIR}/app_config.c
            ${WEB_SRC_DIR}/.build_version
            ${WEB_GEN_DIR}/app_version.h
            ${WEB_GEN_DIR}/app_version.txt
    COMMENT "Generating project version (0.<cfg>.<build>)"
    VERBATIM)

add_custom_target(web_assets_blob ALL
    BYPRODUCTS ${WEB_BLOB}
    COMMAND bash ${WEB_SRC_DIR}/build.sh ${WEB_IMAGE_DIR} ${WEB_GEN_DIR}
    COMMAND node ${CMAKE_SOURCE_DIR}/tools/pack-web.mjs ${WEB_IMAGE_DIR} ${WEB_BLOB}
    WORKING_DIRECTORY ${WEB_SRC_DIR}
    COMMENT "Building Vue frontend and packing embedded web assets"
    USES_TERMINAL
    VERBATIM)
# Version is computed before the web build (so Vite reads the same string) and
# before the firmware compiles (so app_version.h exists for web_server.c).
add_dependencies(web_assets_blob carviston_version)
add_dependencies(${COMPONENT_LIB} carviston_version)
target_include_directories(${COMPONENT_LIB} PRIVATE ${WEB_GEN_DIR})

# Embed the archive into the firmware. Depending on the target above guarantees
# the blob is rebuilt and fresh before it is linked in.
target_add_binary_data(${COMPONENT_LIB} ${WEB_BLOB} BINARY DEPENDS web_assets_blob)
