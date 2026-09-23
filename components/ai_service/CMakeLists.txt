idf_component_register(
    SRCS
        "src/ai_service.c"
        "src/ai_service_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        sys_utils
        storage_manager
        http_router
        idle_face
        cjson
        esp_http_client
        esp-tls
        mbedtls
        esp_timer
)
