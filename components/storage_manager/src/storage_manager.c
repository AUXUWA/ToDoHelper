/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "storage_manager.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "th_bsp.h"

static const char *TAG = "storage";

ESP_EVENT_DEFINE_BASE(STORAGE_EVENT);

#define STORAGE_PATH_MAX 256

/** Warn once when free space drops below this. */
#define STORAGE_LOW_SPACE_KB 64

/** 落盘用的「新一代内容」后缀（写入过程中的临时件）。 */
#define STORAGE_SUFFIX_NEW ".new"

/** 落盘用的「上一代内容」后缀（正式文件的备份）。 */
#define STORAGE_SUFFIX_BAK ".bak"

/** 恢复扫描时单个目录最多处理的条目数。 */
#define STORAGE_SCAN_MAX 32

/** 恢复扫描的目录深度（app 根 → 一级子目录 → 文件）。 */
#define STORAGE_SCAN_DEPTH 2

/** 升级迁移到片内时的单文件上限。 */
#define STORAGE_MIGRATE_MAX_FILE (256 * 1024)

/** 升级迁移到片内时的总量上限（片内分区只有 2 MB）。 */
#define STORAGE_MIGRATE_MAX_TOTAL (1024 * 1024)

static bool s_ready;
static bool s_sd;            /**< 存储根是不是 SD 卡 */
static bool s_int_mounted;   /**< 内置 LittleFS（pinned 后端）是否已挂载 */
static bool s_pinned_ready;  /**< pinned 目录是否可用 */
static char s_root[16];
static char s_app_dir[STORAGE_PATH_MAX];
static char s_pinned_dir[STORAGE_PATH_MAX];
static uint64_t s_total_bytes;
static uint64_t s_free_bytes;
static bool s_low_space_reported;
static SemaphoreHandle_t s_io_lock;

/* ------------------------------------------------------------------------- */
/* Path helpers                                                              */
/* ------------------------------------------------------------------------- */

esp_err_t storage_path(char *buf, size_t len, const char *rel)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }

    int written;
    if (rel == NULL || rel[0] == '\0') {
        written = snprintf(buf, len, "%s", s_app_dir);
    } else if (rel[0] == '/') {
        /* Already rooted: keep the caller honest by ignoring the leading slash. */
        written = snprintf(buf, len, "%s/%s", s_app_dir, rel + 1);
    } else {
        written = snprintf(buf, len, "%s/%s", s_app_dir, rel);
    }

    if (written < 0 || (size_t)written >= len) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t storage_abspath(const char *rel, char *buf, size_t len)
{
    return storage_path(buf, len, rel);
}

/** 读整个文本文件到堆缓冲（调用者负责 free）。定义见文件下方。 */
static esp_err_t file_read_text(const char *abs, char **out, size_t *len);

/**
 * @brief 拼 "<dir>/<rel>"，不经过存储根解析（pinned / 迁移 / 恢复扫描用）。
 */
static esp_err_t join_path(char *buf, size_t len, const char *dir, const char *rel)
{
    if (buf == NULL || len == 0 || dir == NULL || rel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int written = snprintf(buf, len, "%s/%s", dir, rel);
    if (written < 0 || (size_t)written >= len) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/**
 * @brief 文件是否存在，可选返回大小。
 */
static bool file_exists(const char *abs, size_t *size)
{
    struct stat st;
    if (stat(abs, &st) != 0) {
        return false;
    }
    if (size != NULL) {
        *size = (size_t)st.st_size;
    }
    return true;
}

/**
 * @brief 路径是否以给定后缀结尾。
 */
static bool has_suffix(const char *path, const char *suffix)
{
    if (path == NULL || suffix == NULL) {
        return false;
    }
    const size_t plen = strlen(path);
    const size_t slen = strlen(suffix);
    return plen > slen && strcmp(path + plen - slen, suffix) == 0;
}

/* ------------------------------------------------------------------------- */
/* IO 串行化                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * 文件操作整体串行化。
 *
 * 双槽落盘协议（写 .new → 轮换 .bak → 提升）必须整套执行，两个任务同时写
 * 同一个文件会把中间状态搅乱。锁是递归的：storage_read_text() 之类会调用
 * 别的加锁函数，不会自锁。
 */
static bool io_lock(void)
{
    if (s_io_lock == NULL) {
        return false;
    }
    return xSemaphoreTakeRecursive(s_io_lock, portMAX_DELAY) == pdTRUE;
}

static void io_unlock(void)
{
    if (s_io_lock != NULL) {
        (void)xSemaphoreGiveRecursive(s_io_lock);
    }
}

/* ------------------------------------------------------------------------- */
/* Directory helpers                                                          */
/* ------------------------------------------------------------------------- */

/**
 * Create one directory level, treating "already there" as success.
 *
 * The errno check is deliberately not `errno == EEXIST`. FatFs's f_mkdir()
 * returns FR_EXIST only when the path ends with a separator; for a plain path
 * that already names a directory it sets NS_NONAME|NS_DOT and returns
 * **FR_INVALID_NAME**, which the ESP-IDF FATFS VFS maps to EINVAL (vfs_fat.c
 * `fresult_to_errno`). Believing errno alone therefore reported every existing
 * directory as a hard failure, so the real check is a stat().
 */
static esp_err_t mkdir_one(const char *path)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        if (mkdir(path, 0777) == 0) {
            return ESP_OK;
        }
        const int err = errno;

        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Already exists (as a directory), which is exactly what we want. */
            return ESP_OK;
        }

        if (attempt == 0) {
            /* Creating a child immediately after its parent can fail while the
             * parent's directory entry is still only in the FatFs window cache;
             * one short retry is enough and costs nothing. */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        SYS_LOGW(TAG, "mkdir '%s' failed (errno %d)", path, err);
        return ESP_FAIL;
    }
    return ESP_FAIL;
}

/** mkdir -p，作用于绝对路径。 */
static esp_err_t mkdir_p_abs(const char *abs)
{
    char path[STORAGE_PATH_MAX];
    const size_t n = strlen(abs);
    if (n == 0 || n >= sizeof(path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(path, abs, n + 1);

    for (size_t i = 1; i < n; i++) {
        if (path[i] != '/') {
            continue;
        }
        path[i] = '\0';
        /* Log each level's outcome at DEBUG so a partially created tree is
         * immediately visible in the boot trace. */
        esp_err_t err = mkdir_one(path);
        SYS_LOGD(TAG, "mkdir -p level '%s' -> %s", path, esp_err_to_name(err));
        path[i] = '/';
        if (err != ESP_OK) {
            return err;
        }
    }

    esp_err_t err = mkdir_one(path);
    SYS_LOGD(TAG, "mkdir -p leaf '%s' -> %s", path, esp_err_to_name(err));
    return err;
}

esp_err_t storage_mkdir_p(const char *rel)
{
    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }
    return mkdir_p_abs(path);
}

/**
 * Log what is actually on the card root. Cheap, and it is the fastest way to
 * tell "the tree was never created" from "the tree is there but a write failed".
 */
static void storage_log_root_listing(void)
{
    DIR *dir = opendir(s_root);
    if (dir == NULL) {
        SYS_LOGW(TAG, "cannot list '%s' (errno %d)", s_root, errno);
        return;
    }

    char names[256];
    size_t used = 0;
    names[0] = '\0';

    struct dirent *entry = NULL;
    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < 24) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const size_t len = strlen(entry->d_name);
        if (used + len + 2 >= sizeof(names)) {
            break;
        }
        memcpy(names + used, entry->d_name, len);
        used += len;
        names[used++] = ' ';
        names[used] = '\0';
        count++;
    }
    closedir(dir);

    SYS_LOGI(TAG, "%s contains %d entr%s: %s", s_root, count, (count == 1) ? "y" : "ies",
             count > 0 ? names : "(empty)");
}

/* ------------------------------------------------------------------------- */
/* Mount / unmount                                                           */
/* ------------------------------------------------------------------------- */

static void storage_probe_space(void)
{
    s_total_bytes = 0;
    s_free_bytes = 0;

    if (!s_ready) {
        return;
    }

    if (s_sd) {
        uint64_t total = 0;
        uint64_t freeb = 0;
        if (esp_vfs_fat_info(s_root, &total, &freeb) == ESP_OK) {
            s_total_bytes = total;
            s_free_bytes = freeb;
        }
    } else {
        size_t total = 0;
        size_t used = 0;
        if (esp_littlefs_info(STORAGE_INT_PARTITION, &total, &used) == ESP_OK) {
            s_total_bytes = total;
            s_free_bytes = (total > used) ? (uint64_t)(total - used) : 0;
        }
    }

    if (s_total_bytes > 0 && s_free_bytes / 1024 < STORAGE_LOW_SPACE_KB) {
        if (!s_low_space_reported) {
            s_low_space_reported = true;
            SYS_LOGW(TAG, "low space: %llu KB free", (unsigned long long)(s_free_bytes / 1024));
            uint32_t free_kb = (uint32_t)(s_free_bytes / 1024);
            sys_event_post_to(STORAGE_EVENT, STORAGE_EVENT_LOW_SPACE, &free_kb, sizeof(free_kb));
        }
    } else {
        s_low_space_reported = false;
    }

    /* 片内分区独立于存储根：pinned 的文件（待办清单）写在那儿，
     * 空间不够要单独提醒，否则表现为「写盘失败但根目录还有空间」。 */
    if (s_pinned_ready && s_sd) {
        size_t total = 0;
        size_t used = 0;
        if (esp_littlefs_info(STORAGE_INT_PARTITION, &total, &used) == ESP_OK) {
            const size_t freeb = (total > used) ? (total - used) : 0;
            SYS_LOGI(TAG, "pinned partition: %u/%u KB used", (unsigned)(used / 1024),
                     (unsigned)(total / 1024));
            if (freeb / 1024 < STORAGE_LOW_SPACE_KB) {
                SYS_LOGW(TAG, "pinned partition low space: %u KB free",
                         (unsigned)(freeb / 1024));
            }
        }
    }
}

static esp_err_t storage_try_sd(void)
{
    esp_err_t err = th_bsp_sdcard_mount();
    if (err != ESP_OK) {
        SYS_LOGI(TAG, "SD card not available (%s)", esp_err_to_name(err));
        return err;
    }

    SYS_LOGI(TAG, "SD card mounted at %s", STORAGE_SD_ROOT);
    return ESP_OK;
}

/**
 * 挂载内置 LittleFS。
 *
 * 它有两个身份：SD 缺席时的存储根，以及 SD 在场时的 pinned 后端
 * （待办清单固定住它，见 storage_write_cache_pinned()）。两个身份都要求
 * 这个分区随时可用，所以无论 SD 在不在都要挂。
 */
static esp_err_t storage_try_internal(void)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_INT_ROOT,
        .partition_label = STORAGE_INT_PARTITION,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = true,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        SYS_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_int_mounted = true;
    SYS_LOGI(TAG, "internal LittleFS mounted at %s", STORAGE_INT_ROOT);
    return ESP_OK;
}

/** Directory tree from docs/TODOHELPER_REQUIREMENTS.md 9.2. */
static const char *const s_app_subdirs[] = {
    "fonts",
    "images/weather",
    "anim/idle_face",
    "words",
    "ai",
    "tts",
    "cache",
    "backup",
    "logs",
};

static void storage_prepare_tree(void)
{
    size_t failed = 0;
    for (size_t i = 0; i < sizeof(s_app_subdirs) / sizeof(s_app_subdirs[0]); i++) {
        if (storage_mkdir_p(s_app_subdirs[i]) != ESP_OK) {
            failed++;
        }
    }

    if (failed > 0) {
        SYS_LOGW(TAG, "%u of %u application directories could not be created",
                 (unsigned)failed, (unsigned)(sizeof(s_app_subdirs) / sizeof(s_app_subdirs[0])));
    } else {
        SYS_LOGI(TAG, "application directory tree ready under '%s'", s_app_dir);
    }
    storage_log_root_listing();
}

/**
 * 片内 pinned 目录只需要 cache/：待办清单固定写在那里。
 */
static void storage_prepare_pinned_tree(void)
{
    if (!s_pinned_ready) {
        return;
    }
    if (strcmp(s_pinned_dir, s_app_dir) == 0) {
        /* 存储根本身就是片内（没插卡），目录树已经建过了。 */
        return;
    }

    char path[STORAGE_PATH_MAX];
    if (join_path(path, sizeof(path), s_pinned_dir, "cache") != ESP_OK) {
        return;
    }
    const esp_err_t err = mkdir_p_abs(path);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "pinned cache dir '%s' not ready: %s", path, esp_err_to_name(err));
    } else {
        SYS_LOGI(TAG, "pinned cache dir ready at '%s'", path);
    }
}

/** Mirror the storage state into sys_status for the status bar and home page. */
static void storage_status_mutator(sys_status_t *st, void *arg)
{
    const bool *sd = (const bool *)arg;
    st->sd_available = *sd;
    st->storage_available = (storage_root() != NULL);
}

static void storage_publish_status(void)
{
    const bool sd = s_sd;
    /* Without this the UI keeps showing "无存储" even though /sdcard is mounted,
     * because nothing else ever writes these two fields. */
    (void)sys_status_update(storage_status_mutator, (void *)&sd);
}

/* ------------------------------------------------------------------------- */
/* 启动恢复：把 .new / .bak 收敛回正式文件                                 */
/* ------------------------------------------------------------------------- */

/**
 * @brief 处理一个残留槽位文件。
 *
 * 规则（掉电可能停在协议的任何一步）：
 *  - 正式文件在 + 有 .new → .new 是半截货或已被取代，丢掉；
 *  - 正式文件不在 + 有 .new → .new 是完整的新内容，提上来；
 *  - 正式文件不在 + 有 .bak → 只有上一代内容，用它顶上。
 */
static void storage_recover_slot(const char *path, const char *suffix)
{
    if (!has_suffix(path, suffix)) {
        return;
    }

    char target[STORAGE_PATH_MAX];
    const size_t plen = strlen(path);
    const size_t slen = strlen(suffix);
    if (plen - slen >= sizeof(target)) {
        return;
    }
    memcpy(target, path, plen - slen);
    target[plen - slen] = '\0';

    const bool have_target = file_exists(target, NULL);
    if (have_target) {
        if (strcmp(suffix, STORAGE_SUFFIX_NEW) == 0) {
            /* 正式文件还在，.new 是没写完或已被取代的中间件。 */
            remove(path);
            SYS_LOGI(TAG, "dropped stale '%s' (formal file is present)", path);
        }
        return;
    }

    size_t size = 0;
    if (!file_exists(path, &size)) {
        return;
    }

    if (strcmp(suffix, STORAGE_SUFFIX_NEW) == 0) {
        if (size == 0) {
            remove(path);
            SYS_LOGW(TAG, "dropped empty '%s'", path);
            return;
        }
        if (rename(path, target) == 0) {
            SYS_LOGW(TAG, "recovered '%s' from interrupted write", target);
            return;
        }
        /* 提不上来就原样留着，别毁掉内容：下个 boot 还能再试一次。 */
        SYS_LOGW(TAG, "cannot promote '%s' to '%s' (errno %d)", path, target, errno);
        return;
    }

    /* .bak：正式文件不见了，只能拿上一代顶上 */
    if (rename(path, target) == 0) {
        SYS_LOGW(TAG, "restored '%s' from its backup", target);
    }
}

/**
 * @brief 扫描一棵目录树，处理残留的 .new / .bak。
 *
 * 名字先收集再处理：FatFs 与 LittleFS 的目录游标在扫描途中被 rename 打断
 * 会漏项（LittleFS 的 dir 句柄会重新 seek）。
 */
static void storage_recover_tree(const char *dir, int depth)
{
    if (depth > STORAGE_SCAN_DEPTH) {
        return;
    }

    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }

    char *names[STORAGE_SCAN_MAX];
    size_t count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(d)) != NULL && count < STORAGE_SCAN_MAX) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const size_t len = strlen(entry->d_name);
        char *copy = malloc(len + 1);
        if (copy == NULL) {
            break;
        }
        memcpy(copy, entry->d_name, len + 1);
        names[count++] = copy;
    }
    closedir(d);

    /* 子目录先扫（.new/.bak 都在文件那一层，这里只是保持顺序稳定）。 */
    for (size_t i = 0; i < count; i++) {
        char path[STORAGE_PATH_MAX];
        if (join_path(path, sizeof(path), dir, names[i]) != ESP_OK) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            storage_recover_tree(path, depth + 1);
        }
    }

    /* 先处理 .new 再处理 .bak：新内容优先，只有没有新内容时才用备份顶。 */
    for (int pass = 0; pass < 2; pass++) {
        const char *suffix = (pass == 0) ? STORAGE_SUFFIX_NEW : STORAGE_SUFFIX_BAK;
        for (size_t i = 0; i < count; i++) {
            char path[STORAGE_PATH_MAX];
            if (join_path(path, sizeof(path), dir, names[i]) != ESP_OK) {
                continue;
            }
            storage_recover_slot(path, suffix);
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
}

/* ------------------------------------------------------------------------- */
/* 升级迁移：SD 上的旧 cache 搬到片内 pinned                                */
/* ------------------------------------------------------------------------- */

/**
 * @brief 复制一个文件（经由 .new 临时件，保证目标是整份写完的）。
 */
static esp_err_t file_copy_atomic(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return ESP_FAIL;
    }

    char tmp[STORAGE_PATH_MAX + 8];
    const int n = snprintf(tmp, sizeof(tmp), "%s" STORAGE_SUFFIX_NEW, dst);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        fclose(in);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *out = fopen(tmp, "wb");
    if (out == NULL) {
        fclose(in);
        return ESP_FAIL;
    }

    char buf[1024];
    bool failed = false;
    for (;;) {
        const size_t got = fread(buf, 1, sizeof(buf), in);
        if (got == 0) {
            if (ferror(in)) {
                failed = true;
            }
            break;
        }
        if (fwrite(buf, 1, got, out) != got) {
            failed = true;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) {
        failed = true;
    }

    if (failed) {
        remove(tmp);
        return ESP_FAIL;
    }

    remove(dst);
    if (rename(tmp, dst) != 0) {
        SYS_LOGW(TAG, "promote '%s' -> '%s' failed (errno %d)", tmp, dst, errno);
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 升级兼容：SD 侧 cache/ 里的旧文件（老版本的待办清单）在片内还没有时，
 *        搬一份到片内。
 *
 * 只在「SD 是存储根」且「片内已挂载」时做；SD 上的原件保留，
 * 万一片内那份后来出问题还有得退。
 */
static void storage_migrate_cache_to_pinned(void)
{
    if (!s_sd || !s_pinned_ready) {
        return;
    }
    if (strcmp(s_pinned_dir, s_app_dir) == 0) {
        return;
    }

    char src_dir[STORAGE_PATH_MAX];
    char dst_dir[STORAGE_PATH_MAX];
    if (join_path(src_dir, sizeof(src_dir), s_app_dir, "cache") != ESP_OK ||
        join_path(dst_dir, sizeof(dst_dir), s_pinned_dir, "cache") != ESP_OK) {
        return;
    }

    DIR *d = opendir(src_dir);
    if (d == NULL) {
        return;
    }

    char *names[STORAGE_SCAN_MAX];
    size_t count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(d)) != NULL && count < STORAGE_SCAN_MAX) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const size_t len = strlen(entry->d_name);
        char *copy = malloc(len + 1);
        if (copy == NULL) {
            break;
        }
        memcpy(copy, entry->d_name, len + 1);
        names[count++] = copy;
    }
    closedir(d);

    size_t moved = 0;
    for (size_t i = 0; i < count; i++) {
        /*
         * 协议临时件不是数据：.tmp 是旧协议（写 tmp → remove → rename）留下的，
         * .new / .bak 是双槽协议的工作文件。迁移它们只会让片内多一份没人读的
         * 垃圾，还会每次开机重写一次（时间戳比较在 LittleFS 上不可靠）。
         * 真正有价值的那一份——旧协议掉电时用户「丢失」的清单——由 todo_manager
         * 认领（它知道哪个文件是清单、也知道怎么校验它是不是合法 JSON）。
         */
        if (has_suffix(names[i], STORAGE_SUFFIX_NEW) ||
            has_suffix(names[i], STORAGE_SUFFIX_BAK) ||
            has_suffix(names[i], ".tmp")) {
            continue;
        }

        char src[STORAGE_PATH_MAX];
        char dst[STORAGE_PATH_MAX];
        if (join_path(src, sizeof(src), src_dir, names[i]) != ESP_OK ||
            join_path(dst, sizeof(dst), dst_dir, names[i]) != ESP_OK) {
            continue;
        }

        size_t size = 0;
        if (!file_exists(src, &size) || size == 0) {
            continue;
        }

        /*
         * 片内已经有同名文件时**不覆盖**：「两份哪一份更新」不是存储层能回答的
         * 问题——两个后端的文件时间戳都不可靠（LittleFS 不保留源文件时间戳），
         * 拿 mtime 猜会猜错（实测：卡上是 4 条、内部是 2 条，却因为内部时间戳
         * “更新”而留了旧的那份）。
         * 合并两份的判断交给有领域知识的 todo_manager：清单自己带 updated_at
         * 与 next_id，它知道哪一份更「新」。它选中的那一份会被写回片内。
         */
        if (file_exists(dst, NULL)) {
            continue;
        }

        if (size > STORAGE_MIGRATE_MAX_FILE) {
            SYS_LOGW(TAG, "skip migrating '%s': %u bytes over the limit", names[i],
                     (unsigned)size);
            continue;
        }
        if (moved + size > STORAGE_MIGRATE_MAX_TOTAL) {
            SYS_LOGW(TAG, "skip migrating '%s': internal partition budget exhausted",
                     names[i]);
            break;
        }

        if (file_copy_atomic(src, dst) == ESP_OK) {
            moved += size;
            SYS_LOGI(TAG, "migrated '%s' (%u bytes) from the SD card to the internal partition",
                     names[i], (unsigned)size);
        } else {
            SYS_LOGW(TAG, "failed to migrate '%s' to the internal partition", names[i]);
        }
    }

    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }

    if (moved > 0) {
        SYS_LOGI(TAG, "migrated %u bytes of cache into the internal partition", (unsigned)moved);
    }
}

/* ------------------------------------------------------------------------- */
/* Init                                                                       */
/* ------------------------------------------------------------------------- */

esp_err_t storage_manager_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_sd = false;
    s_int_mounted = false;
    s_pinned_ready = false;
    s_root[0] = '\0';
    s_app_dir[0] = '\0';
    s_pinned_dir[0] = '\0';
    s_low_space_reported = false;

    if (s_io_lock == NULL) {
        s_io_lock = xSemaphoreCreateRecursiveMutex();
        if (s_io_lock == NULL) {
            SYS_LOGE(TAG, "cannot create the storage IO lock");
        }
    }

    /* SD 优先做存储根；片内 LittleFS 一定要挂上——它要么是存储根，
     * 要么是待办清单的固定落点（pinned）。 */
    const esp_err_t sd_err = storage_try_sd();
    const esp_err_t int_err = storage_try_internal();

    if (sd_err != ESP_OK) {
        if (int_err != ESP_OK) {
            SYS_LOGE(TAG, "no usable storage backend; running without persistence");
            storage_publish_status();
            sys_event_post_to(STORAGE_EVENT, STORAGE_EVENT_UNMOUNTED, NULL, 0);
            return ESP_OK;
        }
        /* Requirement 12: fall back to the internal filesystem, never abort boot. */
        s_sd = false;
        strlcpy(s_root, STORAGE_INT_ROOT, sizeof(s_root));
    } else {
        s_sd = true;
        strlcpy(s_root, STORAGE_SD_ROOT, sizeof(s_root));
    }

    snprintf(s_app_dir, sizeof(s_app_dir), "%s/%s", s_root, STORAGE_APP_DIR);
    s_ready = true;

    if (s_int_mounted) {
        snprintf(s_pinned_dir, sizeof(s_pinned_dir), "%s/%s", STORAGE_INT_ROOT, STORAGE_APP_DIR);
        s_pinned_ready = true;
    } else {
        /* 片内挂不上：pinned 退化成当前存储根（功能不坏，但不再「固定」）。 */
        s_pinned_ready = false;
        SYS_LOGW(TAG, "internal partition unavailable; pinned files fall back to '%s'",
                 s_app_dir);
    }

    storage_prepare_tree();
    storage_prepare_pinned_tree();

    /* 启动恢复：把上次掉电留下的 .new / .bak 收敛回正式文件。 */
    storage_recover_tree(s_app_dir, 0);
    if (s_pinned_ready && strcmp(s_pinned_dir, s_app_dir) != 0) {
        storage_recover_tree(s_pinned_dir, 0);
    }

    /* 升级兼容：把 SD 上的旧 cache 搬到片内。 */
    storage_migrate_cache_to_pinned();

    storage_probe_space();
    storage_publish_status();

    sys_event_post_to(STORAGE_EVENT, STORAGE_EVENT_MOUNTED, s_root, strlen(s_root) + 1);
    sys_events_post(SYS_EVENT_STORAGE_READY, NULL, 0);

    SYS_LOGI(TAG, "ready: root=%s app=%s pinned=%s free=%llu KB", s_root, s_app_dir,
             s_pinned_ready ? s_pinned_dir : "(none)",
             (unsigned long long)(s_free_bytes / 1024));
    return ESP_OK;
}

esp_err_t storage_manager_deinit(void)
{
    if (!s_ready) {
        return ESP_OK;
    }

    if (s_sd) {
        th_bsp_sdcard_unmount();
    }
    if (s_int_mounted) {
        esp_vfs_littlefs_unregister(STORAGE_INT_PARTITION);
        s_int_mounted = false;
    }

    s_ready = false;
    s_sd = false;
    s_pinned_ready = false;
    s_root[0] = '\0';
    s_app_dir[0] = '\0';
    s_pinned_dir[0] = '\0';
    sys_event_post_to(STORAGE_EVENT, STORAGE_EVENT_UNMOUNTED, NULL, 0);
    return ESP_OK;
}

const char *storage_root(void)
{
    return s_ready ? s_root : NULL;
}

bool storage_sd_available(void)
{
    return s_ready && s_sd;
}

bool storage_available(void)
{
    return s_ready;
}

const char *storage_app_dir(void)
{
    return s_ready ? s_app_dir : NULL;
}

const char *storage_pinned_app_dir(void)
{
    if (!s_ready) {
        return NULL;
    }
    return s_pinned_ready ? s_pinned_dir : s_app_dir;
}

bool storage_pinned_available(void)
{
    return s_ready && s_pinned_ready;
}

uint64_t storage_free_bytes(void)
{
    storage_probe_space();
    return s_free_bytes;
}

uint64_t storage_total_bytes(void)
{
    storage_probe_space();
    return s_total_bytes;
}

/* ------------------------------------------------------------------------- */
/* File operations                                                           */
/* ------------------------------------------------------------------------- */

esp_err_t storage_read_file(const char *rel, void *buf, size_t *len)
{
    if (rel == NULL || buf == NULL || len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        io_unlock();
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const size_t capacity = *len;
    const size_t got = fread(buf, 1, capacity, f);
    const bool short_read = ferror(f) != 0;
    fclose(f);
    io_unlock();

    if (short_read) {
        return ESP_FAIL;
    }

    *len = got;
    return ESP_OK;
}

/**
 * @brief 双槽落盘：X.new → X（旧的转成 X.bak）。
 *
 * FatFs 的 f_rename() 在目标已存在时返回 FR_EXIST（ESP-IDF 的 vfs_fat.c 把它
 * 映射成 EEXIST），所以「删掉 remove 再覆盖式 rename」在 SD 卡上必然失败；
 * LittleFS 的 lfs_rename_() 倒是同一次 commit 里带 DELETE+CREATE，本身就是
 * 原子替换。为了两个后端行为一致，这里统一走两步：
 *
 *   1. 新内容写进 X.new（此刻 X 还是完整的旧内容）；
 *   2. X 改名成 X.bak（失败就到此为止，X 还在，数据没丢）；
 *   3. X.new 改名成 X（此刻 X 已经不存在，FatFs 才肯改名）。
 *
 * 掉电落在 2 与 3 之间时 X 没了，但 X.new 与 X.bak 都在，启动恢复会把 X.new
 * 提上来（见 storage_recover_tree()）。每个名字只留一个 .new / .bak，
 * 不会无限增长。
 */
static esp_err_t file_write_atomic(const char *abs, const void *buf, size_t len)
{
    char p_new[STORAGE_PATH_MAX + 8];
    char p_bak[STORAGE_PATH_MAX + 8];
    int n = snprintf(p_new, sizeof(p_new), "%s" STORAGE_SUFFIX_NEW, abs);
    if (n < 0 || (size_t)n >= sizeof(p_new)) {
        return ESP_ERR_INVALID_SIZE;
    }
    n = snprintf(p_bak, sizeof(p_bak), "%s" STORAGE_SUFFIX_BAK, abs);
    if (n < 0 || (size_t)n >= sizeof(p_bak)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(p_new, "wb");
    if (f == NULL) {
        SYS_LOGW(TAG, "open '%s' for write failed (errno %d)", p_new, errno);
        return ESP_FAIL;
    }

    if (len > 0 && buf != NULL) {
        if (fwrite(buf, 1, len, f) != len) {
            fclose(f);
            remove(p_new);
            SYS_LOGW(TAG, "short write on '%s'", p_new);
            return ESP_FAIL;
        }
    }

    if (fclose(f) != 0) {
        remove(p_new);
        SYS_LOGW(TAG, "flush of '%s' failed", p_new);
        return ESP_FAIL;
    }

    /* 正式文件存在才需要轮换备份；不存在时直接提升 .new。 */
    if (file_exists(abs, NULL)) {
        /* FatFs 的 rename 不覆盖已存在的目标，所以先删掉上一代 .bak。
         * 这一步失败最多丢一份历史备份，abs 本身还在。 */
        remove(p_bak);
        if (rename(abs, p_bak) != 0) {
            SYS_LOGW(TAG, "rotate '%s' -> '%s' failed (errno %d); previous file kept",
                     abs, p_bak, errno);
            remove(p_new);
            return ESP_FAIL;
        }
    }

    if (rename(p_new, abs) != 0) {
        SYS_LOGW(TAG, "promote '%s' -> '%s' failed (errno %d)", p_new, abs, errno);
        /* 正式文件刚被挪走，换回来；换不回来就靠启动恢复兜底。 */
        if (file_exists(p_bak, NULL) && rename(p_bak, abs) != 0) {
            SYS_LOGW(TAG, "rollback '%s' -> '%s' failed (errno %d)", p_bak, abs, errno);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t storage_write_file(const char *rel, const void *buf, size_t len)
{
    if (rel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = file_write_atomic(path, buf, len);
    io_unlock();
    return err;
}

esp_err_t storage_append_file(const char *rel, const void *buf, size_t len)
{
    if (rel == NULL || (buf == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        io_unlock();
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        io_unlock();
        return ESP_FAIL;
    }
    const bool ok = (fclose(f) == 0);
    io_unlock();
    return ok ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 读整个文本文件到堆缓冲（调用者负责 free）。
 */
static esp_err_t file_read_text(const char *abs, char **out, size_t *len)
{
    size_t size = 0;
    if (!file_exists(abs, &size)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (size > 1024 * 1024) {
        SYS_LOGW(TAG, "'%s' too large (%u bytes)", abs, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(size + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(abs, "rb");
    if (f == NULL) {
        free(buf);
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const size_t got = fread(buf, 1, size, f);
    const bool bad = ferror(f) != 0;
    fclose(f);

    if (bad) {
        free(buf);
        return ESP_FAIL;
    }

    buf[got] = '\0';
    *out = buf;
    if (len != NULL) {
        *len = got;
    }
    return ESP_OK;
}

esp_err_t storage_read_text(const char *rel, char **out, size_t *len)
{
    if (rel == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    err = file_read_text(path, out, len);
    io_unlock();
    return err;
}

esp_err_t storage_delete_file(const char *rel)
{
    if (rel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 三个槽位一起删：只删正式文件的话，启动恢复会拿 .bak 把它「复活」。 */
    char slot[STORAGE_PATH_MAX + 8];
    const size_t len = strlen(path);
    if (len + sizeof(STORAGE_SUFFIX_NEW) < sizeof(slot)) {
        snprintf(slot, sizeof(slot), "%s" STORAGE_SUFFIX_NEW, path);
        remove(slot);
    }
    if (len + sizeof(STORAGE_SUFFIX_BAK) < sizeof(slot)) {
        snprintf(slot, sizeof(slot), "%s" STORAGE_SUFFIX_BAK, path);
        remove(slot);
    }

    if (remove(path) != 0) {
        const int err_no = errno;
        io_unlock();
        return (err_no == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    io_unlock();
    return ESP_OK;
}

esp_err_t storage_exists(const char *rel, size_t *size)
{
    if (rel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (size != NULL) {
        *size = (size_t)st.st_size;
    }
    return ESP_OK;
}

/**
 * @brief 正式文件旁边是否留着一份 .bak（上一代内容）。
 */
bool storage_backup_exists(const char *rel)
{
    if (rel == NULL || !s_ready) {
        return false;
    }

    char path[STORAGE_PATH_MAX];
    if (storage_path(path, sizeof(path), rel) != ESP_OK) {
        return false;
    }

    char bak[STORAGE_PATH_MAX + 8];
    const int n = snprintf(bak, sizeof(bak), "%s" STORAGE_SUFFIX_BAK, path);
    if (n < 0 || (size_t)n >= sizeof(bak)) {
        return false;
    }
    return file_exists(bak, NULL);
}

/**
 * @brief 把 <rel>.bak 提升回 <rel>。
 *
 * 主文件内容坏了（解析失败）时由调用方决定要不要走这条路：
 * storage 层只保证 .bak 一直在，不替调用方判断「坏没坏」。
 */
esp_err_t storage_promote_backup(const char *rel)
{
    if (rel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    char bak[STORAGE_PATH_MAX + 8];
    const int n = snprintf(bak, sizeof(bak), "%s" STORAGE_SUFFIX_BAK, path);
    if (n < 0 || (size_t)n >= sizeof(bak)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!file_exists(bak, NULL)) {
        io_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* 坏掉的主文件留成 .corrupt，方便事后看看到底坏了什么。 */
    char corrupt[STORAGE_PATH_MAX + 16];
    if (file_exists(path, NULL)) {
        snprintf(corrupt, sizeof(corrupt), "%s.corrupt", path);
        remove(corrupt);
        rename(path, corrupt);
    }

    if (rename(bak, path) != 0) {
        SYS_LOGW(TAG, "promote backup '%s' -> '%s' failed (errno %d)", bak, path, errno);
        io_unlock();
        return ESP_FAIL;
    }

    io_unlock();
    SYS_LOGW(TAG, "rolled '%s' back to its previous version", path);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* Cache convenience                                                         */
/* ------------------------------------------------------------------------- */

esp_err_t storage_read_cache(const char *name, char **out, size_t *len)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char rel[64];
    int n = snprintf(rel, sizeof(rel), "cache/%s", name);
    if (n < 0 || (size_t)n >= sizeof(rel)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return storage_read_text(rel, out, len);
}

esp_err_t storage_write_cache(const char *name, const void *buf, size_t len)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char rel[64];
    int n = snprintf(rel, sizeof(rel), "cache/%s", name);
    if (n < 0 || (size_t)n >= sizeof(rel)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Requirement 9.2: a cache write failure must never break the main flow. */
    esp_err_t err = storage_write_file(rel, buf, len);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "cache write '%s' failed: %s", rel, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 固定写片内 LittleFS 的缓存文件。
 *
 * 「pinned」= 不随存储根切换：待办清单必须留在一个固定介质上，
 * 否则插卡 / 拔卡会让清单看起来凭空消失。
 */
esp_err_t storage_write_cache_pinned(const char *name, const void *buf, size_t len)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char rel[64];
    int n = snprintf(rel, sizeof(rel), "cache/%s", name);
    if (n < 0 || (size_t)n >= sizeof(rel)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char abs[STORAGE_PATH_MAX];
    if (join_path(abs, sizeof(abs), storage_pinned_app_dir(), rel) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = file_write_atomic(abs, buf, len);
    io_unlock();

    if (err != ESP_OK) {
        SYS_LOGW(TAG, "pinned cache write '%s' failed: %s", rel, esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 读片内 LittleFS 的缓存文件（pinned）。
 *
 * **只读片内那一份**：读不到就是读不到，不回退到存储根。
 * 「两份都在时哪一份更新」不是存储层能回答的问题——两个后端的文件时间戳都不可靠
 * （LittleFS 不保留源文件时间戳），而这层数据有它自己的领域判据
 * （待办清单自己带 updated_at / next_id）。
 * 合并两份的判断在 `todo_manager` 里做（见 todo_manager_init 的双份比较）。
 */
esp_err_t storage_read_cache_pinned(const char *name, char **out, size_t *len)
{
    if (name == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = NULL;

    char rel[64];
    int n = snprintf(rel, sizeof(rel), "cache/%s", name);
    if (n < 0 || (size_t)n >= sizeof(rel)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char abs[STORAGE_PATH_MAX];
    if (join_path(abs, sizeof(abs), storage_pinned_app_dir(), rel) != ESP_OK) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!io_lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = file_read_text(abs, out, len);
    io_unlock();
    return err;
}
