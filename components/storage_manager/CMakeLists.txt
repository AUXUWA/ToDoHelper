idf_component_register(
    SRCS "src/storage_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES sys_utils bsp esp_partition fatfs
    PRIV_REQUIRES littlefs
)
