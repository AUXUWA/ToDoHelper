idf_component_register(
    SRCS
        "src/th_bsp.c"
        "src/th_bsp_input.c"
        "src/th_bsp_periph.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_utils esp32_s31_korvo esp_lvgl_adapter lvgl
    PRIV_REQUIRES button esp_codec_dev
)
