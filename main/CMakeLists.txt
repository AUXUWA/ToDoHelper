# UI assets now live in components/ui_assets (it owns the partition packing and
# the generated mmap header), so main/ only orchestrates.

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        sys_utils
        storage_manager
        bsp
        ui_assets
        http_router
        ui_core
        ui_pages
        wifi_manager
        todo_manager
        calendar_service
        weather_service
        word_service
        sync_server
        idle_face
        ai_service
        voice_service
)
