idf_component_register(
    SRCS
        "src/weather_service.c"
        "src/weather_service_http.c"
        "src/weather_provider_openmeteo.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        sys_utils
        storage_manager
        http_router
        cjson
        esp_http_client
        esp-tls
        mbedtls
        esp_timer
)
