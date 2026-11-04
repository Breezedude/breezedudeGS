# Breezedude FANET ground station - ESP-IDF port
#
# Build:   idf.py set-target esp32s3   (or esp32c6)
#          idf.py build flash monitor
#
# The web UI in www/ is minified + gzipped into a LittleFS image that is
# flashed into the "www" partition alongside the application.
cmake_minimum_required(VERSION 3.16)

set(SUPPORTED_TARGETS esp32s3 esp32c6)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(breezedude_gs)

# ---------------------------------------------------------------------------
# Static web assets -> LittleFS image for the "www" partition
# ---------------------------------------------------------------------------
set(WWW_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/www)
set(WWW_DATA_DIR ${CMAKE_BINARY_DIR}/wwwdata)

# CONFIGURE_DEPENDS: adding or removing a web asset re-runs CMake by itself.
file(GLOB WWW_SOURCES CONFIGURE_DEPENDS ${WWW_SRC_DIR}/*)

# The image carries the firmware version it belongs to (version.txt), so a
# station can tell whether its web UI matches the running firmware.
add_custom_command(
    OUTPUT  ${WWW_DATA_DIR}/.stamp
    COMMAND ${python} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_www.py
            ${WWW_SRC_DIR} ${WWW_DATA_DIR} ${CONFIG_APP_PROJECT_VER}
    DEPENDS ${WWW_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_www.py
            ${CMAKE_BINARY_DIR}/config/sdkconfig.h
    COMMENT "Minifying and compressing web assets"
    VERBATIM
)
add_custom_target(www_assets DEPENDS ${WWW_DATA_DIR}/.stamp)

littlefs_create_partition_image(www ${WWW_DATA_DIR} FLASH_IN_PROJECT DEPENDS www_assets)
