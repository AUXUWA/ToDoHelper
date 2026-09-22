set(COFFEE_UI_DIR "${CMAKE_CURRENT_LIST_DIR}/../ui")
set(COFFEE_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets/coffee"
    CACHE PATH "SquareLine Coffee demo assets directory")

file(GLOB COFFEE_UI_SRCS
    "${COFFEE_UI_DIR}/*.c"
    "${COFFEE_UI_DIR}/components/*.c"
    "${COFFEE_UI_DIR}/screens/*.c"
    "${COFFEE_UI_DIR}/fonts/*.c"
)

idf_component_register(
    SRCS
        "main.c"
        "coffee_assets.c"
        "factory_demo_audio.c"
        "factory_demo_camera.c"
        "factory_demo_usb_hid.c"
        ${COFFEE_UI_SRCS}
    INCLUDE_DIRS "."
        "${COFFEE_UI_DIR}"
        "${COFFEE_UI_DIR}/components"
        "${COFFEE_UI_DIR}/screens"
    REQUIRES esp32_s31_korvo esp_timer heap esp_driver_ppa esp_mm esp_lvgl_adapter esp_lv_decoder esp_mmap_assets
)

if(NOT EXISTS "${COFFEE_ASSETS_DIR}")
    message(FATAL_ERROR "Coffee assets directory not found: ${COFFEE_ASSETS_DIR}")
endif()

spiffs_create_partition_assets(coffee_pjpg "${COFFEE_ASSETS_DIR}" FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".png"
    MMAP_SUPPORT_PJPG
    IMPORT_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_dependencies(${COMPONENT_LIB} assets_coffee_pjpg_bin)
