idf_component_register(
    SRCS "src/debug_shell.c"
    INCLUDE_DIRS "include"
    # L4, same layer as sync_server: it needs HTTP routes (L1) *and* the UI
    # (L2 ui_core) plus the service APIs, and it must not be depended upon by
    # anything - it is a leaf that only app_main pulls in.
    REQUIRES sys_utils http_router ui_core storage_manager bsp esp_app_format cjson
    # ai_service: the debug shell must not write the AI key behind the service's
    # back (it keeps a cached "is a key configured" flag) and must not read the
    # key back out (requirement 8.8). L4 -> L3 is fine.
    ai_service
)
