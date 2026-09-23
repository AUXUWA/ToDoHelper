# The scalable CJK subset is deliberately NOT embedded in the application image.
#
# It is a 1.2 MB TTF, which was 30% of the whole app (4.16 MB) and was rewritten by
# esptool on every single flash - while the same file already sits on the SD card at
# <root>/todohelper/fonts/ui_cjk.ttf, which is where ui_fonts.c loads it from.
#
# To ship a self-contained image (no SD card needed), re-add the embed here:
#
#     set(UI_CORE_EMBED_FILES "")
#     if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/fonts/ui_cjk.ttf")
#         list(APPEND UI_CORE_EMBED_FILES "fonts/ui_cjk.ttf")
#     endif()
#     ... EMBED_FILES ${UI_CORE_EMBED_FILES}
#
# and restore the "write the embedded font to storage" branch in src/ui_fonts.c
# (ft_ensure_file()). Both halves must change together or the link fails on the
# undefined _binary_ui_cjk_ttf_start symbol. See AGENT.md 9.7.

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
)
