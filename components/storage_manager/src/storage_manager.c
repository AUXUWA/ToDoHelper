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
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "th_bsp.h"

static const char *TAG = "storage";

ESP_EVENT_DEFINE_BASE(STORAGE_EVENT);

#define STORAGE_PATH_MAX 256

/** Warn once when free space drops below this. */
#define STORAGE_LOW_SPACE_KB 64

static bool s_ready;
static bool s_sd;
static char s_root[16];
static char s_app_dir[STORAGE_PATH_MAX];
static uint64_t s_total_bytes;
static uint64_t s_free_bytes;
static bool s_low_space_reported;

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

esp_err_t storage_mkdir_p(const char *rel)
{
    char path[STORAGE_PATH_MAX];
    esp_err_t err = storage_path(path, sizeof(path), rel);
    if (err != ESP_OK) {
        return err;
    }

    const size_t n = strlen(path);
    for (size_t i = 1; i < n; i++) {
        if (path[i] != '/') {
            continue;
        }
        path[i] = '\0';
        /* Log each level's outcome at DEBUG so a partially created tree is
         * immediately visible in the boot trace. */
        err = mkdir_one(path);
        SYS_LOGD(TAG, "mkdir -p level '%s' -> %s", path, esp_err_to_name(err));
        path[i] = '/';
        if (err != ESP_OK) {
            return err;
        }
    }

    err = mkdir_one(path);
    SYS_LOGD(TAG, "mkdir -p leaf '%s' -> %s", path, esp_err_to_name(err));
    return err;
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
}

static esp_err_t storage_try_sd(void)
{
    esp_err_t err = th_bsp_sdcard_mount();
    if (err != ESP_OK) {
        SYS_LOGI(TAG, "SD card not available (%s)", esp_err_to_name(err));
        return err;
    }

    s_sd = true;
    strlcpy(s_root, STORAGE_SD_ROOT, sizeof(s_root));
    SYS_LOGI(TAG, "SD card mounted at %s", s_root);
    return ESP_OK;
}

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

    s_sd = false;
    strlcpy(s_root, STORAGE_INT_ROOT, sizeof(s_root));
    SYS_LOGI(TAG, "internal LittleFS mounted at %s", s_root);
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

esp_err_t storage_manager_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_sd = false;
    s_root[0] = '\0';
    s_app_dir[0] = '\0';
    s_low_space_reported = false;

    esp_err_t err = storage_try_sd();
    if (err != ESP_OK) {
        /* Requirement 12: fall back to the internal filesystem, never abort boot. */
        err = storage_try_internal();
    }

    if (err != ESP_OK) {
        SYS_LOGE(TAG, "no usable storage backend; running without persistence");
        storage_publish_status();
        sys_event_post_to(STORAGE_EVENT, STORAGE_EVENT_UNMOUNTED, NULL, 0);
        return ESP_OK;
    }

    snprintf(s_app_dir, sizeof(s_app_dir), "%s/%s", s_root, STORAGE_APP_DIR);
    s_ready = true;

    storage_prepare_tree();
    storage_probe_space();
    storage_publish_status();

    sys_event_post_to(STORAGE_EVENT, STORAGE_EVENT_MOUNTED, s_root, strlen(s_root) + 1);
    sys_events_post(SYS_EVENT_STORAGE_READY, NULL, 0);

    SYS_LOGI(TAG, "ready: root=%s app=%s free=%llu KB", s_root, s_app_dir,
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
    } else {
        esp_vfs_littlefs_unregister(STORAGE_INT_PARTITION);
    }

    s_ready = false;
    s_sd = false;
    s_root[0] = '\0';
    s_app_dir[0] = '\0';
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

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    const size_t capacity = *len;
    const size_t got = fread(buf, 1, capacity, f);
    const bool short_read = ferror(f) != 0;
    fclose(f);

    if (short_read) {
        return ESP_FAIL;
    }

    *len = got;
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

    /* Atomic replace: a power loss mid-write must not corrupt the previous file
     * (critical for cache/todos.json and words/progress.json). */
    char tmp[STORAGE_PATH_MAX + 8];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        SYS_LOGW(TAG, "open '%s' for write failed (errno %d)", tmp, errno);
        return ESP_FAIL;
    }

    if (len > 0 && buf != NULL) {
        if (fwrite(buf, 1, len, f) != len) {
            fclose(f);
            remove(tmp);
            SYS_LOGW(TAG, "short write on '%s'", tmp);
            return ESP_FAIL;
        }
    }

    if (fclose(f) != 0) {
        remove(tmp);
        return ESP_FAIL;
    }

    remove(path);
    if (rename(tmp, path) != 0) {
        SYS_LOGW(TAG, "rename '%s' -> '%s' failed (errno %d)", tmp, path, errno);
        remove(tmp);
        return ESP_FAIL;
    }

    return ESP_OK;
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

    FILE *f = fopen(path, "ab");
    if (f == NULL) {
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return ESP_FAIL;
    }
    return (fclose(f) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_read_text(const char *rel, char **out, size_t *len)
{
    if (rel == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    size_t size = 0;
    esp_err_t err = storage_exists(rel, &size);
    if (err != ESP_OK) {
        return err;
    }
    if (size > 1024 * 1024) {
        SYS_LOGW(TAG, "'%s' too large (%u bytes)", rel, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(size + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t got = size;
    err = storage_read_file(rel, buf, &got);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[got] = '\0';

    *out = buf;
    if (len != NULL) {
        *len = got;
    }
    return ESP_OK;
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
    if (remove(path) != 0) {
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
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
