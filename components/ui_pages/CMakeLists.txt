idf_component_register(
    SRCS
        "src/ui_pages.c"
        "src/page_common.c"
        "src/page_splash.c"
        "src/page_home.c"
        "src/page_placeholders.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ui_core sys_utils lvgl
)
