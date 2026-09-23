# The `assets` partition is produced here rather than in main/, so that the
# generated mmap header exists before this component is compiled.
set(TODOHELPER_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../assets/ui")

idf_component_register(
    SRCS "src/ui_assets.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES sys_utils lvgl esp_lvgl_adapter esp_mmap_assets
)

if(NOT EXISTS "${TODOHELPER_ASSETS_DIR}")
    message(FATAL_ERROR "UI assets directory not found: ${TODOHELPER_ASSETS_DIR}")
endif()

# MMAP_FILE_SUPPORT_FORMAT must list every extension that should end up in the
# image; the packer only scans the top level of the directory.
spiffs_create_partition_assets(assets "${TODOHELPER_ASSETS_DIR}" FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".png"
    IMPORT_INC_PATH "${CMAKE_CURRENT_LIST_DIR}/src"
)

add_dependencies(${COMPONENT_LIB} assets_assets_bin)
