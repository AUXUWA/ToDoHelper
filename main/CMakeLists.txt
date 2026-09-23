# UI assets packed into the `assets` partition (see partitions.csv).
set(TODOHELPER_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets/ui")

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        sys_utils
        storage_manager
        bsp
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
)

if(NOT EXISTS "${TODOHELPER_ASSETS_DIR}")
    message(FATAL_ERROR "UI assets directory not found: ${TODOHELPER_ASSETS_DIR}")
endif()

spiffs_create_partition_assets(assets "${TODOHELPER_ASSETS_DIR}" FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".png"
    IMPORT_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_dependencies(${COMPONENT_LIB} assets_assets_bin)
