idf_component_register(
    SRCS "src/http_router.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_utils esp_http_server
)
