# Optional scalable CJK font embedded into the application image.
# Regenerate with tools/gen_cjk_font.py (see README.md).
set(UI_CORE_EMBED_FILES "")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/fonts/ui_cjk.ttf")
    list(APPEND UI_CORE_EMBED_FILES "fonts/ui_cjk.ttf")
endif()

idf_component_register(
    SRCS
        "src/ui_core.c"
        "src/ui_theme.c"
        "src/ui_fonts.c"
        "src/ui_statusbar.c"
        "src/ui_widgets.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES sys_utils bsp storage_manager lvgl esp_lvgl_adapter
    EMBED_FILES ${UI_CORE_EMBED_FILES}
)
