idf_component_register(
    SRCS
        "app/bd_app.cpp"
        "board/bd_board.cpp"
        "core/bd_state.cpp"
        "core/bd_auth.cpp"
        "core/bd_migrate.cpp"
        "core/bd_stats.cpp"
        "radio/bd_repeat.cpp"
        "core/types.cpp"
        "aprs/aprs.cpp"
        "protocols/flarm.cpp"
        "protocols/adsl.cpp"
        "protocols/manchester.cpp"
        "radio/bd_radio.cpp"
        "net/bd_net.cpp"
        "net/bd_diag.cpp"
        "web/bd_http.cpp"
        "web/bd_ws_api.cpp"
        "web/bd_ws_out.cpp"
        "web/bd_snapshot.cpp"
        "web/bd_console.cpp"
        "power/bd_power.cpp"
        "storage/bd_storage.cpp"
        "storage/bd_fwstore.cpp"
        "ota/bd_fwupdate.cpp"
        "ota/ota_gs.cpp"
        "ota/config_gs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        compat
        app_update
        bootloader_support
        driver
        esp_app_format
        esp_http_client
        esp_http_server
        esp_https_ota
        esp_netif
        esp_partition
        esp_pm
        esp_timer
        esp_wifi
        bblanchon__arduinojson
        jgromes__radiolib
        joltwallet__littlefs
        lwip
        mbedtls
        nvs_flash
        spi_flash
)

# The ported protocol modules still use the macro names the PlatformIO build
# passed as -D flags; they are fed from Kconfig here so there is one place to
# configure them.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BREEZEDUDE_GS_ENABLE_LORA_OTA=$<IF:$<BOOL:${CONFIG_BDGS_ENABLE_LORA_OTA}>,1,0>
    BREEZEDUDE_GS_ENABLE_FSK_RX=$<IF:$<BOOL:${CONFIG_BDGS_ENABLE_FSK_RX}>,1,0>
    # ArduinoJson only auto-detects the Arduino types when ARDUINO is defined;
    # tell it explicitly that our String/Print/Stream from the compat layer are
    # the Arduino ones, otherwise serializeJson(doc, String) does not resolve.
    # Print/Stream support stays off: nothing serializes into a Print and the
    # compat Stream has no Printable counterpart.
    ARDUINOJSON_ENABLE_ARDUINO_STRING=1
    ARDUINOJSON_ENABLE_ARDUINO_PRINT=0
    ARDUINOJSON_ENABLE_ARDUINO_STREAM=0
    ARDUINOJSON_ENABLE_PROGMEM=0
    OTA_GS_FAST_BW_KHZ=500
    OTA_GS_FAST_SF=7
    OTA_GS_FAST_CR=5
    OTA_GS_FAST_PREAMBLE=6
    OTA_GS_FAST_SYNCWORD=0x12
)

if(CONFIG_BDGS_ENABLE_LORA_OTA)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        "OTA_GS_BACKEND_URL=\"${CONFIG_BDGS_OTA_BACKEND_URL}\""
        "OTA_GS_STATS_URL=\"${CONFIG_BDGS_OTA_STATS_URL}\""
    )
endif()

# The partition tables this firmware can run with, embedded so a station on an
# older layout can be migrated into one of them (core/bd_migrate.cpp): the 4 MB
# table, and the same plus a second image store for boards with 8 MB. Generated
# with the same options as IDF's own partition-table.bin.
idf_build_get_property(project_dir PROJECT_DIR)
idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(python PYTHON)
# Certificate roots for the HTTPS diagnostic reports (net/bd_diag.cpp).
target_add_binary_data(${COMPONENT_LIB} "certs/isrg_roots.pem" TEXT)

foreach(size 4MB 8MB)
    string(TOLOWER ${size} lower)
    set(csv "${project_dir}/partitions_${lower}.csv")
    set(bin "${CMAKE_CURRENT_BINARY_DIR}/target_partitions_${lower}.bin")
    add_custom_command(
        OUTPUT ${bin}
        COMMAND ${python} ${idf_path}/components/partition_table/gen_esp32part.py -q
                --offset ${CONFIG_PARTITION_TABLE_OFFSET}
                --primary-bootloader-offset ${CONFIG_BOOTLOADER_OFFSET_IN_FLASH}
                --flash-size ${size}
                ${csv} ${bin}
        DEPENDS ${csv} ${idf_path}/components/partition_table/gen_esp32part.py
        VERBATIM
    )
    add_custom_target(bd_target_partitions_${lower} DEPENDS ${bin})
    target_add_binary_data(${COMPONENT_LIB} ${bin} BINARY DEPENDS bd_target_partitions_${lower})
endforeach()

# Protocol decoders and the OTA gateway are tight loops over small buffers; the
# size optimisation of the rest of the project costs real receive performance
# here, so these files are built for speed.
set_source_files_properties(
    protocols/flarm.cpp protocols/adsl.cpp protocols/manchester.cpp core/types.cpp
    PROPERTIES COMPILE_OPTIONS "-O2"
)
