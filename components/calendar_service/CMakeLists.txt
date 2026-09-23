idf_component_register(
    SRCS
        "src/calendar_service.c"
        "src/calendar_service_http.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_utils esp_netif http_router lwip cjson
)
