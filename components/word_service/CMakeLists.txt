idf_component_register(
    SRCS
        "src/word_service.c"
        "src/word_service_http.c"
        "src/word_default_book.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES sys_utils storage_manager http_router cjson
)
