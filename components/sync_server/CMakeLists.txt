idf_component_register(
    SRCS "src/sync_server.c"
    INCLUDE_DIRS "include"
    REQUIRES
        sys_utils
        http_router
        cjson
        wifi_manager
        storage_manager
        esp_app_format
        esp_timer
        esp_hw_support
        esp_system
        heap
    PRIV_REQUIRES mdns
    EMBED_FILES "web/index.html"
)
