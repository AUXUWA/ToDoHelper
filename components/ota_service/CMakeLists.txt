idf_component_register(
    SRCS "src/ota_service.c" "src/ota_service_http.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_utils http_router
    PRIV_REQUIRES app_update esp_http_server esp_timer
)
