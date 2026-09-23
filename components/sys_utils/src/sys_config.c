/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sys_utils.h"

#include <stdlib.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sys_config";

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static bool s_ready;
static char s_namespace[16] = CONFIG_TODOHELPER_NVS_NAMESPACE;

/** Longest string we ever store in NVS; guards against a corrupt length field. */
#define SYS_CONFIG_STR_MAX 2048

static esp_err_t sys_config_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t sys_config_init(const char *nvs_namespace)
{
    esp_err_t err = sys_config_lock();
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (nvs_namespace != NULL && nvs_namespace[0] != '\0') {
        strlcpy(s_namespace, nvs_namespace, sizeof(s_namespace));
    }

    if (s_ready) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    /* Bring up the NVS partition here so that no component ever calls
     * nvs_flash_init() / nvs_open() on its own (requirements 7.13). */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        SYS_LOGW(TAG, "NVS needs erase (%s), reformatting", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        SYS_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open(s_namespace, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        SYS_LOGE(TAG, "nvs_open('%s') failed: %s", s_namespace, esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "config ready (namespace '%s')", s_namespace);
    return ESP_OK;
}

static esp_err_t sys_config_require_ready(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t sys_config_get_str(const char *key, char *out, size_t len)
{
    if (key == NULL || out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sys_config_require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (sys_config_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t required = 0;
    err = nvs_get_str(s_nvs, key, NULL, &required);
    if (err == ESP_OK) {
        if (required == 0 || required > SYS_CONFIG_STR_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (required > len) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = nvs_get_str(s_nvs, key, out, &required);
        }
    }
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

esp_err_t sys_config_get_str_alloc(const char *key, char **out)
{
    if (key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    esp_err_t err = sys_config_require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (sys_config_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t required = 0;
    err = nvs_get_str(s_nvs, key, NULL, &required);
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (required == 0 || required > SYS_CONFIG_STR_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc(required);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = nvs_get_str(s_nvs, key, buf, &required);
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    *out = buf;
    return ESP_OK;
}

esp_err_t sys_config_set_str(const char *key, const char *val)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sys_config_require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (sys_config_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Never log the value: keys such as wifi_pass / ai_api_key / auth_token are secret. */
    if (val == NULL) {
        err = nvs_erase_key(s_nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    } else {
        if (strlen(val) >= SYS_CONFIG_STR_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = nvs_set_str(s_nvs, key, val);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        SYS_LOGW(TAG, "set '%s' failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t sys_config_get_i32(const char *key, int32_t *out)
{
    if (key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sys_config_require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (sys_config_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = nvs_get_i32(s_nvs, key, out);
    xSemaphoreGive(s_lock);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return err;
}

esp_err_t sys_config_set_i32(const char *key, int32_t val)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sys_config_require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (sys_config_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = nvs_set_i32(s_nvs, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t sys_config_get_bool(const char *key, bool *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int32_t v = 0;
    esp_err_t err = sys_config_get_i32(key, &v);
    if (err != ESP_OK) {
        return err;
    }
    *out = (v != 0);
    return ESP_OK;
}

esp_err_t sys_config_set_bool(const char *key, bool val)
{
    return sys_config_set_i32(key, val ? 1 : 0);
}

esp_err_t sys_config_erase(const char *key)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sys_config_require_ready();
    if (err != ESP_OK) {
        return err;
    }
    if (sys_config_lock() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    err = nvs_erase_key(s_nvs, key);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

int32_t sys_config_get_i32_or(const char *key, int32_t fallback)
{
    int32_t v = fallback;
    if (sys_config_get_i32(key, &v) != ESP_OK) {
        return fallback;
    }
    return v;
}

bool sys_config_get_bool_or(const char *key, bool fallback)
{
    bool v = fallback;
    if (sys_config_get_bool(key, &v) != ESP_OK) {
        return fallback;
    }
    return v;
}

esp_err_t sys_config_get_str_or(const char *key, char *out, size_t len, const char *fallback)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = sys_config_get_str(key, out, len);
    if (err == ESP_ERR_NOT_FOUND) {
        strlcpy(out, fallback ? fallback : "", len);
        return ESP_OK;
    }
    return err;
}
