idf_component_register(
    SRCS "src/voice_service.c"
         "src/voice_wake.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES
        sys_utils
        storage_manager
        bsp
        http_router
        cjson
        esp_http_client
        esp-tls
        mbedtls
        esp_codec_dev
        esp_timer
        esp-sr
)
