idf_component_register(
    SRCS
        "src/todo_manager.c"
        "src/todo_manager_http.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES sys_utils storage_manager http_router cjson
)
