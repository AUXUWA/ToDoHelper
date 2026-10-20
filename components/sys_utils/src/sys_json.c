/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_json.c
 * @brief Keep cJSON's allocations out of internal RAM.
 *
 * **This is the root cause of the "SD card inserted -> boot abort loop" bug**
 * (AGENT.md 9.0.5). The chain, verified on hardware:
 *
 * 1. `sdkconfig.defaults` sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096`, and
 *    `heap_caps_malloc_default()` therefore sends *every* request of <= 4096
 *    bytes to the **internal** heap first and only falls back to PSRAM once that
 *    pool is exhausted (`heap_caps.c`: `malloc_alwaysinternal_limit`).
 * 2. cJSON allocates one ~56-byte node per JSON element, plus a copy of every
 *    key and every string value. The CET-6 word book is 546 KB / 833 entries /
 *    roughly 7000 nodes, so parsing it drives internal RAM to ~0 while it runs.
 * 3. With an SD card inserted that parse happens at boot (the book only exists
 *    on the card). Concurrently the WiFi driver is connecting, and its
 *    `ets_timer_setfn()` runs from the tick hook and calls
 *    `esp_timer_create()` -> `heap_caps_calloc(..., MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)`.
 *    That allocation cannot fall back to PSRAM, so it fails with
 *    `ESP_ERR_NO_MEM`, and because it sits behind `ESP_ERROR_CHECK` the device
 *    aborts (`ets_timer_legacy.c:58`) and reboots in a loop.
 *    Without a card the compiled-in 32-word starter book is parsed instead and
 *    nothing starves, which is why the crash looked card-dependent.
 *
 * Installing these hooks makes cJSON prefer PSRAM (11 MB free here) and fall
 * back to the normal heap, so JSON parsing can no longer starve internal RAM.
 * The hook is process-global, which suits this project: every JSON user
 * (todo/weather/word/wifi/ai/sync/HTTP handlers) handles bulk device data
 * rather than tiny protocol frames, and nothing parses JSON from an ISR or with
 * the flash cache disabled - the two places PSRAM may not be touched.
 *
 * The second job of this file is the *depth* guard (roadmap v1.5.0 B3): cJSON
 * itself only refuses to nest deeper than `CJSON_NESTING_LIMIT` (1000 by
 * default), which is far beyond what this board's task stacks can survive.
 * `sys_json_parse()` therefore measures the nesting depth of the text *before*
 * handing it to cJSON and refuses anything too deep.
 */

#include "sys_utils.h"

#include "cJSON.h"
#include "esp_heap_caps.h"

static const char *TAG = "sys_json";

/** PSRAM first, then whatever the default heap offers, so a build without PSRAM still works. */
static void *json_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

/**
 * `heap_caps_free()` finds the owning heap, so it is also correct for pointers
 * that a component allocated with plain `malloc()` and handed to cJSON (or the
 * other way round: cJSON memory released with plain `free()`).
 */
static void json_free(void *ptr)
{
    heap_caps_free(ptr);
}

/**
 * @brief 幂等地把 cJSON 的分配钩子装上。
 *
 * 钩子是进程全局的，重复安装只是把同样的函数指针再写一遍，无副作用；这里用
 * 原子量只是为了避免两个任务同时初始化时互相踩（哪怕真的踩了也只是重复写
 * 同样的值，不会出错）。
 */
static void json_ensure_hooks(void)
{
    static bool s_installed;

    if (__atomic_load_n(&s_installed, __ATOMIC_ACQUIRE)) {
        return;
    }

    cJSON_Hooks hooks = {
        .malloc_fn = json_malloc,
        .free_fn = json_free,
    };
    cJSON_InitHooks(&hooks);

    __atomic_store_n(&s_installed, true, __ATOMIC_RELEASE);
}

void sys_json_use_psram(void)
{
    json_ensure_hooks();
}

/* ------------------------------------------------------------------------ */
/* 限深解析                                                                  */
/* ------------------------------------------------------------------------ */

/**
 * @brief 扫描 JSON 文本，统计 `{` / `[` 的嵌套深度。
 *
 * 只统计深度，不做语法校验 —— 语法错误交给 cJSON 自己报错。
 *
 * 两个必须处理的细节：
 *
 * 1. **字符串字面量里可以出现括号**。待办标题写成「[急] 交周报」是完全合法的
 *    JSON，扫描时必须跳过引号内的内容，否则会把标题里的 `[` 当成一次嵌套。
 * 2. **转义**。`\"` 是字符串里的一个引号，不是字符串的结束，所以要处理 `\`。
 *
 * 扫描在「根值结束」时立刻停：cJSON 的 `cJSON_Parse()` 用的是
 * `require_null_terminated = 0`，只解析**第一个**完整值、忽略后面的尾随内容
 * （见 cJSON.c 的 `cJSON_ParseWithOpts()`），所以尾随垃圾里的括号不该算进深度。
 *
 * @param json      待扫描文本（必须以 NUL 结尾）
 * @param max_depth 允许的最大嵌套深度
 * @param out_depth 可选；返回实际达到的最大深度（超限时返回触发超限的那一层）
 * @return true 表示深度在限额之内
 */
static bool json_scan_depth(const char *json, int max_depth, int *out_depth)
{
    int depth = 0;
    int deepest = 0;
    bool in_string = false;
    bool escaped = false;

    for (const char *p = json; *p != '\0'; p++) {
        char c = *p;

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
                if (depth == 0) {
                    /* 根值本身就是字符串：cJSON 到这里就解析完了 */
                    break;
                }
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            depth++;
            if (depth > deepest) {
                deepest = depth;
            }
            if (depth > max_depth) {
                if (out_depth != NULL) {
                    *out_depth = depth;
                }
                return false;
            }
        } else if (c == '}' || c == ']') {
            if (depth == 0) {
                /* 多余的闭括号：这份 JSON 本来就不合法，深度已经没意义了 */
                break;
            }
            depth--;
            if (depth == 0) {
                /* 根容器闭合：第一个值到此结束，后面的尾随内容不用管 */
                break;
            }
        }
    }

    if (out_depth != NULL) {
        *out_depth = deepest;
    }
    return true;
}

/**
 * @brief 解析 JSON，但限制嵌套深度。
 *
 * **为什么 cJSON 自带的 1000 层上限在本工程里远远不够**：
 *
 * - cJSON 1.7.19 的 `CJSON_NESTING_LIMIT` 默认 1000
 *   （managed_components/espressif__cjson/cJSON/cJSON.h:137），
 *   而 `parse_array()` / `parse_object()` 是**递归**调用 `parse_value()` 的。
 * - 用 objdump 看编译产物（riscv32-esp-elf-objdump -d
 *   build/esp-idf/espressif__cjson/.../cJSON.c.obj）：`parse_value` 的 prologue 是
 *   `addi sp,sp,-96` —— **每深一层就吃掉 96 字节栈**，1000 层最坏要 ~96 KB。
 * - 而本工程的栈都很小：开机主任务 `CONFIG_ESP_MAIN_TASK_STACK_SIZE=12288`
 *   （sdkconfig.defaults:49）→ 12288 / 96 = **128 层**就爆；
 *   httpd 任务 `http_cfg.stack_size = 6144`（components/sync_server/src/sync_server.c:741）
 *   → **64 层**就爆。
 * - `todos.json`、`review_log.json` 这类文件是用户可以直接在 SD 卡上编辑的，
 *   一个 `[`×200 的畸形文件就能让设备**每次开机都崩**（AGENT.md §9.0.5 那一类故障）。
 *
 * 所以深度必须在**进 cJSON 之前**拦掉，而不是指望 cJSON 自己的 1000 层上限。
 * 这里不改 cJSON 组件本身（`managed_components/espressif__cjson` 是外部依赖，
 * 升级即丢），只在调用前自己扫一遍文本统计深度。
 *
 * @param json      待解析文本，NULL 直接返回 NULL
 * @param max_depth 允许的最大嵌套深度（根容器算第 1 层）。待办这种两三层结构
 *                  给 32 就足够；给小了会误杀合法数据，给大了会逼近栈上限
 * @return 成功返回 cJSON 根节点（**调用方负责 `cJSON_Delete()`**，与 `cJSON_Parse()`
 *         的约定一致）；失败返回 NULL —— 可能是深度超限，也可能是语法错误 /
 *         内存不足。调用方应当走「保留旧数据 / 空数据」的降级路径，**绝不能**
 *         在开机路径上因为解析失败而崩掉。
 */
cJSON *sys_json_parse(const char *json, int max_depth)
{
    int depth = 0;

    if (json == NULL) {
        return NULL;
    }

    /* 保证走的是与 sys_json_use_psram() 同一套 PSRAM 优先的分配钩子 */
    json_ensure_hooks();

    /* 根容器至少占一层，max_depth 传 0 之类的脏值不应该把正常的 {} 也毙掉 */
    if (max_depth < 1) {
        max_depth = 1;
    }

    if (!json_scan_depth(json, max_depth, &depth)) {
        SYS_LOGW(TAG, "json nesting too deep (depth %d > limit %d), refused", depth, max_depth);
        return NULL;
    }

    return cJSON_Parse(json);
}
