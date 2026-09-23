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
 * @brief Write a whole file atomically (tmp file + rename).
 */
esp_err_t storage_write_file(const char *rel, const void *buf, size_t len);

/**
 * @brief Append bytes to a file, creating it when missing.
 */
esp_err_t storage_append_file(const char *rel, const void *buf, size_t len);

/**
 * @brief Read a whole text file into a heap buffer (NUL terminated).
 *        The caller owns the buffer and must free() it.
 */
esp_err_t storage_read_text(const char *rel, char **out, size_t *len);

esp_err_t storage_delete_file(const char *rel);

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
 */
esp_err_t storage_write_cache(const char *name, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif
