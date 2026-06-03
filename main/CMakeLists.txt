# matter_node.cpp collapses to a stub unless CONFIG_CARVISTON_MATTER_ENABLE=y.
# When enabled it includes <esp_matter.h>, so the esp_matter component (which
# transitively brings the chip stack) must be on this component's requirements.
# esp_matter itself reaches the build via EXTRA_COMPONENT_DIRS, exported by
# `source $ESP_MATTER_PATH/export.sh` (see docs/MATTER.md). Guard the dependency
# on the Kconfig flag so the local-only stub build needs neither installed.
set(matter_reqs "")
if(CONFIG_CARVISTON_MATTER_ENABLE)
    list(APPEND matter_reqs esp_matter)
endif()

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
        "matter_node.cpp"
        "event_log.c"
        "benchmark.c"
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
        ${matter_reqs}
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

add_custom_target(web_assets_blob ALL
    BYPRODUCTS ${WEB_BLOB}
    COMMAND bash ${WEB_SRC_DIR}/build.sh ${WEB_IMAGE_DIR}
    COMMAND node ${CMAKE_SOURCE_DIR}/tools/pack-web.mjs ${WEB_IMAGE_DIR} ${WEB_BLOB}
    WORKING_DIRECTORY ${WEB_SRC_DIR}
    COMMENT "Building Vue frontend and packing embedded web assets"
    USES_TERMINAL
    VERBATIM)

# Embed the archive into the firmware. Depending on the target above guarantees
# the blob is rebuilt and fresh before it is linked in.
target_add_binary_data(${COMPONENT_LIB} ${WEB_BLOB} BINARY DEPENDS web_assets_blob)
