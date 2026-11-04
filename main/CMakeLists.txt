idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        compat
        bdgs
        esp_app_format
        nvs_flash
)
