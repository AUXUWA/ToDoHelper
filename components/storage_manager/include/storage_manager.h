/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file storage_manager.h
 * @brief L1 storage abstraction: one logical root, two possible backends.
 *
 * Backend selection happens once in storage_manager_init():
 *   1. SD card over SDMMC 4-bit  -> root "/sdcard"
 *   2. internal LittleFS 1.x     -> root "/littlefs"
 *
 * The internal LittleFS partition is mounted in *both* cases: besides being the
 * fallback root it is the "pinned" backend that keeps the todo list on a fixed
 * medium (see storage_write_cache_pinned()).
 *
 * Upper layers must never hardcode "/sdcard": they build paths with
 * storage_path()/storage_abspath() and live with either backend
 * (docs/TODOHELPER_REQUIREMENTS.md 9.2, 12).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#include "sys_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(STORAGE_EVENT);

typedef enum {
    STORAGE_EVENT_MOUNTED = 0,   /**< payload: const char *root */
    STORAGE_EVENT_UNMOUNTED,     /**< payload: none */
    STORAGE_EVENT_LOW_SPACE,     /**< payload: uint32_t free_kb */
} storage_event_id_t;

/** Project directory created under the backend root. */
#define STORAGE_APP_DIR        "todohelper"

/** Mount point used when the SD card is available. */
#define STORAGE_SD_ROOT        "/sdcard"

/** Mount point used when falling back to the internal LittleFS partition. */
#define STORAGE_INT_ROOT       "/littlefs"

/** Partition label of the internal LittleFS partition (see partitions.csv). */
#define STORAGE_INT_PARTITION  "storage"

/**
 * @brief Probe and mount a backend, create the application directory tree.
 *
 * Idempotent. Never returns a hard error for a missing SD card: if both
 * backends fail the component stays in "unavailable" state and every file
 * operation returns ESP_ERR_INVALID_STATE instead of crashing.
 */
esp_err_t storage_manager_init(void);

/**
 * @brief Release the backend. Mainly used by tests.
 */
esp_err_t storage_manager_deinit(void);

/**
 * @brief Root path of the active backend ("/sdcard" or "/littlefs"), or NULL
 *        when no backend could be mounted.
 */
const char *storage_root(void);

/**
 * @brief True when the SD card backend is the active one.
 */
bool storage_sd_available(void);

/**
 * @brief True when any backend is usable.
 */
bool storage_available(void);

/**
 * @brief Absolute path of the application directory, e.g. "/sdcard/todohelper".
 */
const char *storage_app_dir(void);

/**
 * @brief Absolute path of the "pinned" application directory, i.e. the one that
 *        does not follow the storage root, e.g. "/littlefs/todohelper".
 *
 * Falls back to storage_app_dir() when the internal partition could not be
 * mounted (the pinned guarantee is then degraded, not lost).
 */
const char *storage_pinned_app_dir(void);

/**
 * @brief True when the pinned backend (internal LittleFS) is really mounted.
 */
bool storage_pinned_available(void);

/**
 * @brief Build "<app_dir>/<rel>" into @p buf.
 *
 * @param rel Relative path such as "cache/todos.json". NULL or "" yields the
 *            application directory itself.
 */
esp_err_t storage_path(char *buf, size_t len, const char *rel);

/** Convenience wrapper returning a static-free freshly formatted pointer set. */
esp_err_t storage_abspath(const char *rel, char *buf, size_t len);

/**
 * @brief Create every missing directory component of @p rel (like mkdir -p).
 */
esp_err_t storage_mkdir_p(const char *rel);

/**
 * @brief Read a whole file. On entry *len is the buffer capacity; on success it
 *        is the number of bytes read.
 */
esp_err_t storage_read_file(const char *rel, void *buf, size_t *len);

/**
 * @brief Write a whole file with the double-slot protocol.
 *
 * The new content goes to "<rel>.new", the previous file is rotated to
 * "<rel>.bak" and only then "<rel>.new" is promoted to "<rel>". A power loss
 * in between leaves both slots on disk; storage_manager_init() recovers them
 * on the next boot. "<rel>.bak" always holds the previous generation, so a
 * caller that finds "<rel>" unparsable can roll back with
 * storage_promote_backup().
 */
esp_err_t storage_write_file(const char *rel, const void *buf, size_t len);

/**
 * @brief Append bytes to a file, creating it when missing.
 */
esp_err_t storage_append_file(const char *rel, const void *buf, size_t len);

/**
 * @brief Read a whole text file into a heap buffer (NUL terminated).
 *        The caller owns the buffer and must free() it.
 *
 * The buffer is allocated **PSRAM first** (see ROADMAP v1.7.0 B2): a few hundred
 * bytes on the internal heap would chip away at the largest contiguous internal
 * block that the WiFi PHY needs. The returned pointer may therefore live in
 * PSRAM - that is fine, `free()` is equivalent to `heap_caps_free()` in IDF.
 */
esp_err_t storage_read_text(const char *rel, char **out, size_t *len);

esp_err_t storage_delete_file(const char *rel);

/**
 * @brief List the files in a directory.
 *
 * @param rel  directory relative to the app directory ("" = the app dir itself)
 * @param[out] out  heap buffer with the file names, one per line (free() it)
 * @param[out] len  the number of bytes written (optional)
 */
esp_err_t storage_list_dir(const char *rel, char **out, size_t *len);

/**
 * @brief True when a previous generation of "<rel>" is kept as "<rel>.bak".
 */
bool storage_backup_exists(const char *rel);

/**
 * @brief Roll "<rel>" back to "<rel>.bak".
 *
 * The unparsable file is kept as "<rel>.corrupt" for inspection. The storage
 * layer does not decide whether a file is broken - that is the caller's call.
 */
esp_err_t storage_promote_backup(const char *rel);

/**
 * @brief Check existence and optionally report the size.
 *        Returns ESP_ERR_NOT_FOUND when the file does not exist.
 */
esp_err_t storage_exists(const char *rel, size_t *size);

/**
 * @brief Free space of the active backend in bytes; 0 when unknown.
 */
uint64_t storage_free_bytes(void);

/**
 * @brief Total space of the active backend in bytes; 0 when unknown.
 */
uint64_t storage_total_bytes(void);

/**
 * @brief Convenience: read a cache file, returning ESP_ERR_NOT_FOUND when absent.
 *        Used by the offline degradation paths of todo/weather (FR2, FR3).
 */
esp_err_t storage_read_cache(const char *name, char **out, size_t *len);

/**
 * @brief Convenience: write a cache file, logging but not failing the caller.
 *
 * The file lives under "<storage root>/todohelper/cache/". Words, fonts and
 * other bulk data stay on the SD card.
 */
esp_err_t storage_write_cache(const char *name, const void *buf, size_t len);

/**
 * @brief Write a cache file pinned to the internal LittleFS partition.
 *
 * "Pinned" means the path does not follow the storage root: it is always
 * "<internal>/todohelper/cache/<name>", so inserting or removing the SD card
 * cannot make the todo list appear to vanish. Falls back to the storage root
 * when the internal partition could not be mounted.
 */
esp_err_t storage_write_cache_pinned(const char *name, const void *buf, size_t len);

/**
 * @brief Read a cache file pinned to the internal LittleFS partition.
 *
 * When the pinned file does not exist yet (first boot after an upgrade) the
 * copy on the storage root is served instead, so the list does not come up
 * empty; writes always go to the pinned location.
 *
 * @param out Receives a NUL terminated heap buffer the caller must free().
 *             The buffer is allocated PSRAM first (see storage_read_text()).
 */
esp_err_t storage_read_cache_pinned(const char *name, char **out, size_t *len);

#ifdef __cplusplus
}
#endif
