idf_component_register(
    SRCS
        "src/sys_log.c"
        "src/sys_err.c"
        "src/sys_events.c"
        "src/sys_time.c"
        "src/sys_config.c"
        "src/sys_status.c"
        "src/sys_notify.c"
        "src/sys_heap.c"
        "src/sys_json.c"
        "src/sys_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_event nvs_flash esp_timer log esp_common heap esp_hw_support cjson
)
