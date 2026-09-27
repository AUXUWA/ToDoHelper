idf_component_register(
    SRCS
        "src/maimemo_client.c"
        "src/maimemo_http.c"
    INCLUDE_DIRS "include"
    REQUIRES
        sys_utils
        storage_manager
        http_router
        cjson
        esp_http_client
        esp-tls
        mbedtls
)
