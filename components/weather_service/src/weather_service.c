/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "weather_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "storage_manager.h"
#include "sys_net.h"
#include "sys_utils.h"

static const char *TAG = "weather";

ESP_EVENT_DEFINE_BASE(WEATHER_EVENT);

/*
 * wifi_manager 的事件基。
 *
 * weather_service 刻意**不**依赖 wifi_manager 组件：组件之间的 L3 -> L3 依赖
 * 是分层规则不允许的（见 docs/TODOHELPER_FRAMEWORK.md）。所以这里像
 * word_service 那样只声明这个基，用 ESP_EVENT_ANY_ID 订阅，自己判断
 * 「地址是不是刚出现」。
 */
extern esp_event_base_t const WIFI_MGR_EVENT;

#ifndef CONFIG_TODOHELPER_WEATHER_INTERVAL_S
#define CONFIG_TODOHELPER_WEATHER_INTERVAL_S WEATHER_DEFAULT_INTERVAL_S
#endif
#ifndef CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS
#define CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS 12000
#endif

#define WEATHER_CACHE_FILE "weather.json"
/**
 * 天气 worker 的栈。
 *
 * esp_http_client 没有自己的任务，mbedTLS 握手直接吃调用者的栈
 * （IDF v6.1 的 esp_http_client.c 里没有 xTaskCreate）。刷新和 IP 定位
 * 都在这个任务上跑 HTTPS，所以栈要按"跑得下 TLS 握手"来给（规格 P6）。
 */
#define WEATHER_TASK_STACK 8192
#define WEATHER_TASK_PRIO  4
#define WEATHER_REFRESH_WAIT_MS 20000
/**
 * 拿到 IP 之前不发请求，最多等这么久。
 *
 * 真机实测 IP 在开机后约 1 秒就位（v1.6.0 日志：6018 ms 天气 ready，7032 ms 拿到 IP），
 * 6 秒足够覆盖慢一点的握手；到点还没网就**跳过这一轮**（weather_do_refresh()
 * 见到没有地址就直接返回，不会发起注定失败的 DNS + 连接）。
 *
 * **为什么是 6 秒**：同步的 weather_refresh() 只等 WEATHER_REFRESH_WAIT_MS
 * （20 s）。「等网 + 一次 HTTP 尝试」（12 s）必须留在这个预算之内，否则
 * 「POST /api/v1/weather/refresh」会从「200 + 缓存读数」变成
 * 「503 weather API did not answer in time」—— 那是用户可见的行为变化。
 */
#define WEATHER_NET_WAIT_MS 6000
/**
 * 传输失败后隔多久再试一次。
 *
 * 拿到 IP 的那一刻 DNS/路由还没完全就绪（DHCP 把它们和地址一起送到，
 * 但事件比它们早一点点到），马上重试常常还是失败；歇 3 秒足够让它们就位。
 */
#define WEATHER_RETRY_DELAY_MS 3000
/**
 * 只有「很快失败」才值得再试一次。
 *
 * 同步路径 weather_refresh() 只等 WEATHER_REFRESH_WAIT_MS（20 s），
 * 一次 HTTP 尝试最长 12 s —— 所以第一次尝试已经用掉超过 5 秒时，
 * 再加 3 秒间隔 + 12 秒重试就会撑破那个预算，接口会从「回 200 + 缓存读数」
 * 变成「回 503」。那才是真的用户可见变化，不能为了天气多试一次就搭进去。
 */
#define WEATHER_RETRY_BUDGET_MS 5000

/**
 * Default IP geolocation endpoint (FR3 priority 2).
 *
 * ipapi.co answers HTTPS without an API key and reports city/latitude/longitude.
 * Overridable through the NVS key `geo_url`; the parser also accepts the
 * `lat`/`lon` spelling and a nested `location` object, so switching providers is
 * usually just a URL change.
 */
#define WEATHER_GEO_ENDPOINT_DEFAULT "https://ipapi.co/json/"

/** Worker request tokens: 队列里的令牌只表示"跑一轮 worker"。 */
#define WEATHER_REQ_REFRESH 0

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static bool s_initialized;
static weather_data_t s_data;
static char s_city[64];
static char s_provider[24] = WEATHER_PROVIDER_DEFAULT;
static char s_geo_url[192] = WEATHER_GEO_ENDPOINT_DEFAULT;
static uint32_t s_interval_s = CONFIG_TODOHELPER_WEATHER_INTERVAL_S;

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static TaskHandle_t s_task;
static QueueHandle_t s_requests;
static SemaphoreHandle_t s_done;

/**
 * 置位表示"有一次 IP 定位请求待处理"。
 *
 * 不用队列来记这件事：队列深度只有 1，忙的时候塞不进去，直接丢会让用户
 * 点的「重新定位」莫名其妙没反应。标志位不会丢 —— worker 每一轮都会来取。
 */
static bool s_detect_requested;

/**
 * @brief 上一次刷新是不是因为「还没有 IP」而放弃的。
 *
 * 这是「开机就能拿到天气」这件事的最后一环：
 *
 * - worker 在开机时拿到刷新请求，先等 IP（WEATHER_NET_WAIT_MS，最多 6 s）；
 * - 等到了 → 正常刷新，这个标志清掉；
 * - 没等到 → 这一轮放弃（不再白跑一次注定失败的 DNS），标志置位；
 * - 之后 IP 真的到了（DHCP 有时要十几秒）→ 「拿到 IP」的事件把标志读走，
 *   再排一次刷新。
 *
 * 没有这个标志的话，开机时 IP 来得慢一点，天气就会一直显示缓存值，
 * 直到 900 秒的周期定时器下一次唤醒它 —— 用户看到的现象是「联网了但天气不动」。
 */
static bool s_awaiting_ip;

/**
 * @brief 上一次看到 WiFi 事件时有没有地址（用来只对「从无到有」这个跳变反应）。
 */
static bool s_had_ip;

static esp_timer_handle_t s_timer;

/* ------------------------------------------------------------------------ */
/* Provider registry                                                         */
/* ------------------------------------------------------------------------ */

static const weather_provider_t *const s_providers[] = {
    &weather_provider_open_meteo,
};

const char *const *weather_provider_names(size_t *count)
{
    static const char *names[sizeof(s_providers) / sizeof(s_providers[0])];
    static bool filled;

    if (!filled) {
        for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); i++) {
            names[i] = s_providers[i]->name;
        }
        filled = true;
    }
    if (count != NULL) {
        *count = sizeof(s_providers) / sizeof(s_providers[0]);
    }
    return names;
}

const weather_provider_t *weather_provider_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_providers) / sizeof(s_providers[0]); i++) {
        if (strcmp(s_providers[i]->name, name) == 0) {
            return s_providers[i];
        }
    }
    return NULL;
}

const weather_provider_t *weather_provider_current(void)
{
    const weather_provider_t *provider = weather_provider_find(s_provider);
    return (provider != NULL) ? provider : &weather_provider_open_meteo;
}

const char *weather_get_provider(void)
{
    return weather_provider_current()->name;
}

esp_err_t weather_set_provider(const char *name)
{
    if (name == NULL || weather_provider_find(name) == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    strlcpy(s_provider, name, sizeof(s_provider));
    esp_err_t err = sys_config_set_str(SYS_KEY_WEATHER_PROV, s_provider);
    SYS_LOGI(TAG, "provider set to '%s'", s_provider);
    (void)weather_refresh_async();
    return err;
}

/* ------------------------------------------------------------------------ */
/* Shared helpers (weather_internal.h)                                       */
/* ------------------------------------------------------------------------ */

esp_err_t weather_url_encode(const char *in, char *out, size_t out_len)
{
    if (in == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)in[i];
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            if (o + 1 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = (char)c;
        } else {
            /* UTF-8 city names must be percent-encoded byte by byte. */
            if (o + 3 >= out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0x0F];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
    return ESP_OK;
}

esp_err_t weather_read_api_key(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return sys_config_get_str(SYS_KEY_WEATHER_KEY, out, len);
}

/* ------------------------------------------------------------------------ */
/* NVS persistence                                                           */
/* ------------------------------------------------------------------------ */

/**
 * 写 NVS 字符串，但值一个字节没变就不写。
 *
 * nvs_set_str + nvs_commit 是一次 flash 写。天气每 15 分钟刷新一次，
 * 每次都把同样的城市名/坐标写回去，flash 寿命就被白白吃掉，而 NVS 里的
 * 值根本没变。先读回来比一比，相同就什么都不做（规格 P3）。
 *
 * @return 值没变时返回 ESP_OK（视为成功），否则是 NVS 写的返回值。
 */
static esp_err_t config_set_str_if_changed(const char *key, const char *value)
{
    if (key == NULL || value == NULL || value[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    char current[64];
    if (sys_config_get_str(key, current, sizeof(current)) == ESP_OK &&
        strcmp(current, value) == 0) {
        return ESP_OK;
    }
    return sys_config_set_str(key, value);
}

/** Accumulates the response body across HTTP_EVENT_ON_DATA callbacks. */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} http_body_t;

#define WEATHER_MAX_BODY (16 * 1024)

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_body_t *ctx = (http_body_t *)evt->user_data;
    if (ctx == NULL || evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (ctx->overflow) {
        return ESP_OK;
    }
    if (ctx->len + (size_t)evt->data_len >= WEATHER_MAX_BODY) {
        ctx->overflow = true;
        return ESP_OK;
    }

    if (ctx->buf == NULL) {
        ctx->cap = 2048;
        /* PSRAM 优先：内部 RAM 只剩几十 KB，而 CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096
         * 会让普通 malloc 优先落在内部堆上。16 KB 的响应缓冲放在内部堆里，
         * 等于在内存最紧的时候临时吃掉 16 KB（AGENT.md §9.0）。 */
        ctx->buf = heap_caps_malloc_prefer(ctx->cap, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (ctx->buf == NULL) {
            ctx->overflow = true;
            return ESP_OK;
        }
    }
    if (ctx->len + (size_t)evt->data_len + 1 > ctx->cap) {
        size_t new_cap = ctx->cap * 2;
        while (new_cap < ctx->len + (size_t)evt->data_len + 1) {
            new_cap *= 2;
        }
        /* 扩容也要走 PSRAM：普通 realloc 会把块留在原来那块所属的堆上，
         * 不会因为变大就搬到 PSRAM 去。 */
        char *grown = heap_caps_realloc_prefer(ctx->buf, new_cap, 2,
                                               MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
        if (grown == NULL) {
            ctx->overflow = true;
            return ESP_OK;
        }
        ctx->buf = grown;
        ctx->cap = new_cap;
    }

    memcpy(ctx->buf + ctx->len, evt->data, evt->data_len);
    ctx->len += (size_t)evt->data_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

static esp_err_t weather_http_get_json_locked(const char *url, char **out)
{
    http_body_t ctx = { 0 };
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        /* Certificate bundle keeps HTTPS working without pinning a leaf cert. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        heap_caps_free(ctx.buf);
        return err;
    }
    if (ctx.overflow) {
        heap_caps_free(ctx.buf);
        return ESP_ERR_INVALID_SIZE;
    }
    if (status != 200) {
        SYS_LOGW(TAG, "HTTP %d for %s", status, url);
        heap_caps_free(ctx.buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ctx.buf == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = ctx.buf;
    return ESP_OK;
}

esp_err_t weather_http_get_json(const char *url, char **out)
{
    if (url == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    /*
     * One HTTPS at a time (see components/sys_utils/include/sys_net.h).
     *
     * A TLS handshake allocates a lot (mbedTLS sizes the session buffers from
     * MBEDTLS_SSL_IN_CONTENT_LEN / OUT_CONTENT_LEN, 16 KB each), and the
     * internal heap is what the WiFi PHY needs a ~30 KB contiguous block of.
     * Two handshakes at once made mbedTLS fail with -0x008D
     * (PSA_ERROR_INSUFFICIENT_MEMORY) and the request then sat in a 60 s
     * backoff window.
     *
     * The lock is taken around the request only - no SD writes, no LVGL calls.
     */
    const esp_err_t lock_err = sys_net_lock(SYS_NET_LOCK_DEFAULT_MS);
    if (lock_err != ESP_OK) {
        SYS_LOGW(TAG, "another request is holding the network lock");
        return lock_err;
    }

    const esp_err_t err = weather_http_get_json_locked(url, out);
    sys_net_unlock();
    return err;
}

void weather_wmo_to_condition(int code, char *text, size_t text_len,
                              char *icon, size_t icon_len)
{
    const char *condition = "未知";
    const char *name = "w_overcast";

    switch (code) {
    case 0:
        condition = "晴";
        name = "w_sunny";
        break;
    case 1:
        condition = "少云";
        name = "w_cloudy";
        break;
    case 2:
        condition = "多云";
        name = "w_cloudy";
        break;
    case 3:
        condition = "阴";
        name = "w_overcast";
        break;
    case 45:
    case 48:
        condition = "雾";
        name = "w_fog";
        break;
    case 51:
    case 53:
    case 55:
        condition = "毛毛雨";
        name = "w_rain";
        break;
    case 56:
    case 57:
        condition = "冻雨";
        name = "w_rain";
        break;
    case 61:
        condition = "小雨";
        name = "w_rain";
        break;
    case 63:
        condition = "中雨";
        name = "w_rain";
        break;
    case 65:
        condition = "大雨";
        name = "w_rain";
        break;
    case 66:
    case 67:
        condition = "冻雨";
        name = "w_rain";
        break;
    case 71:
        condition = "小雪";
        name = "w_snow";
        break;
    case 73:
        condition = "中雪";
        name = "w_snow";
        break;
    case 75:
        condition = "大雪";
        name = "w_snow";
        break;
    case 77:
        condition = "米雪";
        name = "w_snow";
        break;
    case 80:
        condition = "阵雨";
        name = "w_rain";
        break;
    case 81:
        condition = "强阵雨";
        name = "w_rain";
        break;
    case 82:
        condition = "暴雨";
        name = "w_rain";
        break;
    case 85:
    case 86:
        condition = "阵雪";
        name = "w_snow";
        break;
    case 95:
        condition = "雷阵雨";
        name = "w_thunder";
        break;
    case 96:
    case 99:
        condition = "雷阵雨伴冰雹";
        name = "w_thunder";
        break;
    default:
        break;
    }

    if (text != NULL) {
        strlcpy(text, condition, text_len);
    }
    if (icon != NULL) {
        strlcpy(icon, name, icon_len);
    }
}

/* ------------------------------------------------------------------------ */
/* Cache                                                                     */
/* ------------------------------------------------------------------------ */

esp_err_t weather_save_cache(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const weather_data_t snapshot = s_data;
    const char *city = s_city;
    xSemaphoreGive(s_lock);

    /* Never store the API key here (requirement 8.8). */
    cJSON_AddStringToObject(root, "city", city);
    cJSON_AddNumberToObject(root, "lat", snapshot.lat);
    cJSON_AddNumberToObject(root, "lon", snapshot.lon);
    cJSON_AddNumberToObject(root, "temp", (double)snapshot.temp);
    cJSON_AddNumberToObject(root, "feels_like", (double)snapshot.feels_like);
    cJSON_AddNumberToObject(root, "humidity", snapshot.humidity);
    cJSON_AddStringToObject(root, "condition", snapshot.condition);
    cJSON_AddStringToObject(root, "icon", snapshot.icon);
    cJSON_AddNumberToObject(root, "weather_code", snapshot.weather_code);
    cJSON_AddNumberToObject(root, "update_ts", (double)snapshot.update_ts);
    cJSON_AddStringToObject(root, "provider", weather_provider_current()->name);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t err = storage_write_cache(WEATHER_CACHE_FILE, json, strlen(json));
    cJSON_free(json);
    return err;
}

esp_err_t weather_load_cache(void)
{
    if (!storage_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    char *json = NULL;
    size_t len = 0;
    esp_err_t err = storage_read_cache(WEATHER_CACHE_FILE, &json, &len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    weather_data_t loaded = { 0 };
    const cJSON *item = NULL;

    if ((item = cJSON_GetObjectItemCaseSensitive(root, "city")) != NULL && cJSON_IsString(item)) {
        strlcpy(loaded.city, item->valuestring, sizeof(loaded.city));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "condition")) != NULL && cJSON_IsString(item)) {
        strlcpy(loaded.condition, item->valuestring, sizeof(loaded.condition));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "icon")) != NULL && cJSON_IsString(item)) {
        strlcpy(loaded.icon, item->valuestring, sizeof(loaded.icon));
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "lat")) != NULL && cJSON_IsNumber(item)) {
        loaded.lat = item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "lon")) != NULL && cJSON_IsNumber(item)) {
        loaded.lon = item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "temp")) != NULL && cJSON_IsNumber(item)) {
        loaded.temp = (float)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "feels_like")) != NULL && cJSON_IsNumber(item)) {
        loaded.feels_like = (float)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "humidity")) != NULL && cJSON_IsNumber(item)) {
        loaded.humidity = (int)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "weather_code")) != NULL && cJSON_IsNumber(item)) {
        loaded.weather_code = (int)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "update_ts")) != NULL && cJSON_IsNumber(item)) {
        loaded.update_ts = (int64_t)item->valuedouble;
    }
    cJSON_Delete(root);

    loaded.valid = (loaded.city[0] != '\0') || (loaded.condition[0] != '\0');
    if (!loaded.valid) {
        return ESP_ERR_NOT_FOUND;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = loaded;
    if (loaded.city[0] != '\0' && s_city[0] == '\0') {
        strlcpy(s_city, loaded.city, sizeof(s_city));
    }
    xSemaphoreGive(s_lock);

    SYS_LOGI(TAG, "cache loaded: %s %.1f C '%s'", loaded.city, (double)loaded.temp,
             loaded.condition);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Location                                                                  */
/* ------------------------------------------------------------------------ */

const char *weather_get_geo_endpoint(void)
{
    return s_geo_url;
}

esp_err_t weather_set_geo_endpoint(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        strlcpy(s_geo_url, WEATHER_GEO_ENDPOINT_DEFAULT, sizeof(s_geo_url));
        return sys_config_erase(SYS_KEY_GEO_URL);
    }
    if (strlen(url) >= sizeof(s_geo_url)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(s_geo_url, url, sizeof(s_geo_url));
    return sys_config_set_str(SYS_KEY_GEO_URL, s_geo_url);
}

esp_err_t weather_set_location(double lat, double lon, const char *label)
{
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data.lat = lat;
    s_data.lon = lon;
    if (label != NULL && label[0] != '\0') {
        strlcpy(s_city, label, sizeof(s_city));
    }
    xSemaphoreGive(s_lock);

    if (label != NULL && label[0] != '\0') {
        (void)config_set_str_if_changed(SYS_KEY_CITY, s_city);
    }
    char coord[24];
    snprintf(coord, sizeof(coord), "%.4f", lat);
    (void)config_set_str_if_changed(SYS_KEY_CITY_LAT, coord);
    snprintf(coord, sizeof(coord), "%.4f", lon);
    (void)config_set_str_if_changed(SYS_KEY_CITY_LON, coord);

    SYS_LOGI(TAG, "location set to %.4f, %.4f ('%s')", lat, lon, s_city);
    return ESP_OK;
}

/** Read a number under any of the spellings the supported providers use. */
static bool json_number_any(const cJSON *root, const char *const *keys, size_t key_count,
                            double *out)
{
    for (size_t i = 0; i < key_count; i++) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsNumber(item)) {
            *out = item->valuedouble;
            return true;
        }
    }
    return false;
}

esp_err_t weather_detect_location(void)
{
    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && !st.online && !st.wifi_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char *body = NULL;
    esp_err_t err = weather_http_get_json(s_geo_url, &body);
    if (err != ESP_OK) {
        SYS_LOGW(TAG, "IP geolocation request failed: %s", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(body);
    heap_caps_free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Some providers nest the coordinates under "location". */
    const cJSON *scope = root;
    const cJSON *nested = cJSON_GetObjectItemCaseSensitive(root, "location");
    if (cJSON_IsObject(nested)) {
        scope = nested;
    }

    static const char *const lat_keys[] = { "latitude", "lat" };
    static const char *const lon_keys[] = { "longitude", "lon", "lng" };
    double lat = 0;
    double lon = 0;

    if (!json_number_any(scope, lat_keys, 2, &lat) ||
        !json_number_any(scope, lon_keys, 3, &lon)) {
        /* A provider that reports a failure usually says so in "status"/"error". */
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        SYS_LOGW(TAG, "geolocation response unusable (%s / %s)",
                 cJSON_IsString(status) ? status->valuestring : "?",
                 cJSON_IsString(reason) ? reason->valuestring : "-");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char label[64] = { 0 };
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(scope, "city");
    const cJSON *region = cJSON_GetObjectItemCaseSensitive(scope, "region");
    if (cJSON_IsString(city) && city->valuestring[0] != '\0') {
        strlcpy(label, city->valuestring, sizeof(label));
    } else if (cJSON_IsString(region)) {
        strlcpy(label, region->valuestring, sizeof(label));
    }
    cJSON_Delete(root);

    err = weather_set_location(lat, lon, label[0] != '\0' ? label : NULL);
    if (err != ESP_OK) {
        return err;
    }
    SYS_LOGI(TAG, "located by IP: %s (%.4f, %.4f)", label, lat, lon);
    (void)sys_notify("weather_located", NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------------ */
/* Refresh                                                                   */
/* ------------------------------------------------------------------------ */

/* 定义在下面（和 worker 放在一起）：「设备有没有拿到地址」的判据。 */
static bool weather_network_up(void);

static esp_err_t weather_do_refresh(void)
{
    sys_status_t st;
    if (sys_status_get(&st) == ESP_OK && !st.online && !st.wifi_connected) {
        SYS_LOGD(TAG, "offline, keeping cached reading");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 「关联上了但还没有地址」同样按离线处理。
     *
     * 没有地址时 DNS 根本解析不出来（真机日志里的
     * "getaddrinfo() returns 202"），这时发请求只是白跑一次 DNS 查询 +
     * 一次连接尝试（电台也醒了），换回来的只有一个错误。
     * 提前返回还顺带把同步路径的预算守住：weather_refresh() 等的是
     * WEATHER_REFRESH_WAIT_MS（20 s），而 worker 的等网最多
     * WEATHER_NET_WAIT_MS（6 s）—— 6 + 12（一次 HTTP 尝试）< 20。
     */
    if (!weather_network_up()) {
        SYS_LOGD(TAG, "no IP address yet, keeping cached reading");
        /* 记下「这一轮是因为没地址才放弃的」：地址到了之后要补一次刷新
         * （见 on_wifi_event）。 */
        __atomic_store_n(&s_awaiting_ip, true, __ATOMIC_RELEASE);
        return ESP_ERR_INVALID_STATE;
    }

    const weather_provider_t *provider = weather_provider_current();
    if (provider->needs_api_key) {
        char key[80];
        if (weather_read_api_key(key, sizeof(key)) != ESP_OK || key[0] == '\0') {
            SYS_LOGW(TAG, "provider '%s' needs an API key", provider->name);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    char city[64];
    char resolved[64] = { 0 };
    double lat = 0;
    double lon = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(city, s_city, sizeof(city));
    lat = s_data.lat;
    lon = s_data.lon;
    const bool have_coords = (s_data.lat != 0.0 || s_data.lon != 0.0) &&
                             (strcmp(s_data.city, s_city) == 0);
    xSemaphoreGive(s_lock);

    if (city[0] == '\0') {
        SYS_LOGD(TAG, "no city configured yet");
        return ESP_ERR_NOT_FOUND;
    }

    char label[64] = { 0 };
    if (!have_coords) {
        esp_err_t err = provider->geocode(city, &lat, &lon, label, sizeof(label));
        if (err != ESP_OK) {
            return err;
        }
    }

    weather_data_t fresh = { 0 };
    esp_err_t err = provider->fetch(lat, lon, &fresh);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(fresh.city, label[0] != '\0' ? label : city, sizeof(fresh.city));
    s_data = fresh;
    if (fresh.city[0] != '\0') {
        strlcpy(s_city, fresh.city, sizeof(s_city));
    }
    /* 锁内取一份快照，锁外再写 NVS：s_city 是别的任务也会动的全局缓冲。 */
    strlcpy(resolved, s_city, sizeof(resolved));
    xSemaphoreGive(s_lock);

    /* Remember the resolved place so a later boot can show the right label, and
     * keep the coordinates in NVS for diagnostics/inspection.
     * 值没变就不写：定时刷新每 15 分钟一次，一次无条件的三键重写就是三次
     * flash 写，而城市名和坐标通常一个字节都没变（规格 P3）。 */
    (void)config_set_str_if_changed(SYS_KEY_CITY, resolved);
    char coord[24];
    snprintf(coord, sizeof(coord), "%.4f", lat);
    (void)config_set_str_if_changed(SYS_KEY_CITY_LAT, coord);
    snprintf(coord, sizeof(coord), "%.4f", lon);
    (void)config_set_str_if_changed(SYS_KEY_CITY_LON, coord);

    (void)weather_save_cache();

    (void)sys_event_post_to(WEATHER_EVENT, WEATHER_EVENT_UPDATED, NULL, 0);
    (void)sys_notify("weather_changed", NULL);
    return ESP_OK;
}

/** 记下一次 IP 定位请求。标志位不会因为队列满而丢。 */
static void weather_request_detect(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_detect_requested = true;
    xSemaphoreGive(s_lock);
}

/** 取走一次待处理的 IP 定位请求（没有就返回 false）。 */
static bool weather_take_detect_request(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool requested = s_detect_requested;
    s_detect_requested = false;
    xSemaphoreGive(s_lock);
    return requested;
}

/**
 * @brief 设备是否已经拿到 IP（可以发外网请求了）。
 *
 * 真机启动日志（2026-10-20，v1.6.0）：
 *     I (6018) module 'weather_service' ready
 *     E (6018) esp-tls: couldn't get hostname for :api.open-meteo.com: getaddrinfo() returns 202
 *     W (6042) TH:weather: refresh failed: ESP_ERR_HTTP_CONNECT
 *     I (7032) TH:wifi: got ip 192.168.137.95      ← IP 在这之后才有
 *
 * 开机时的那次刷新是**必然失败**的：连 DNS 都解析不出来。
 * 白跑一次 DNS 查询 + 一次连接尝试（电台也醒了），换回来的只是一个错误。
 *
 * 判据用 sys_status 的 wifi_ip：wifi_manager 在 IP_EVENT_STA_GOT_IP 时会刷新它
 * （components/wifi_manager/src/wifi_manager.c 里 "Keep sys_status fresh" 那段）。
 */
static bool weather_network_up(void)
{
    sys_status_t st;
    if (sys_status_get(&st) != ESP_OK) {
        return false;
    }
    /*
     * 光判「非空」是不够的：wifi_manager 在还没拿到地址时把 "0.0.0.0"
     * 放进 sys_status（wifi_manager.c 里 s_ip 的初值就是 "0.0.0.0"，
     * 关联上了但 DHCP 还没回包时也是它），所以开机之后 sys_status.wifi_ip
     * 一开始就是 "0.0.0.0" —— 非空 → 判据恒为真 → 等网分支一次都进不去。
     * 判据与 word_service 的 have_ip() 用同一套。
     */
    return st.wifi_ip[0] != '\0' && strcmp(st.wifi_ip, "0.0.0.0") != 0;
}

/**
 * @brief 等网络就绪（有 IP）再发请求。
 *
 * 只在这个 worker 任务里等，不阻塞任何调用方（页面上的「刷新」按钮是异步的，
 * 会拿到「刷新中」的提示，等多久都不影响界面）。
 *
 * @param timeout_ms 最长等多久；到点还没网就放弃这一轮
 *                    （weather_do_refresh() 见到没有地址会直接返回，
 *                     不会发起注定失败的 DNS + 连接）。
 * @return true = 有网了。
 */
static bool weather_wait_for_network(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 500) {
        if (weather_network_up()) {
            return true;
        }
        if (waited == 0) {
            SYS_LOGI(TAG, "no IP yet - waiting before the first request");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return weather_network_up();
}

/**
 * @brief WiFi 事件：地址刚出现时，把之前放弃的那次刷新补上。
 *
 * 事件回调跑在共享事件任务上，所以这里**只做最轻的事**：置标志 + 往队列里
 * 塞一个令牌（不阻塞）。真正的 HTTPS 请求在天气 worker 上跑。
 *
 * 只对「从没有地址到有地址」这个跳变反应：
 * - 已经有地址 → 不重复刷（否则一次扫描事件就能白跑一次网络往返）；
 * - 还没有地址 → 什么都不做（等下一个事件）；
 * - 之前那一轮刷新并不是因为没地址才放弃的 → 也不重复刷。
 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)data;

    const bool now = weather_network_up();
    const bool had = s_had_ip;
    s_had_ip = now;

    if (!now || had) {
        return;   /* 还是没有地址，或者地址没变化 */
    }
    if (!__atomic_load_n(&s_awaiting_ip, __ATOMIC_ACQUIRE)) {
        return;   /* 没有被放弃的刷新要补 */
    }

    SYS_LOGI(TAG, "the address arrived - retrying the weather refresh");
    __atomic_store_n(&s_awaiting_ip, false, __ATOMIC_RELEASE);
    (void)weather_refresh_async();
}

static void weather_task(void *arg)
{
    (void)arg;
    uint8_t token;

    while (xQueueReceive(s_requests, &token, portMAX_DELAY) == pdTRUE) {
        /* 令牌只负责把 worker 叫醒；这一轮要不要做 IP 定位看标志位。 */
        (void)token;

        /*
         * 没有 IP 就不发请求。
         *
         * 开机时天气模块比拿到 IP 早一秒多，那一次请求必然失败（DNS 都解析不出来），
         * 白跑一次 DNS + 一次连接。等 IP 的代价是几秒，换来的是「开机就能拿到天气」。
         */
    if (!weather_network_up()) {
        (void)weather_wait_for_network(WEATHER_NET_WAIT_MS);
        /* 等到了就正常刷新；没等到，这一轮会在 weather_do_refresh() 里
         * 记下「等地址」，地址真的到了再补一次。 */
        __atomic_store_n(&s_awaiting_ip, !weather_network_up(), __ATOMIC_RELEASE);
    }

        if (weather_take_detect_request()) {
            /*
             * Wait for a valid clock, don't just skip.
             *
             * TLS certificate validation checks notBefore/notAfter, so a request
             * made before SNTP syncs always fails:
             *
             *   E esp-x509-crt-bundle: Failed to verify certificate
             *   E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x3000
             *
             * (-0x3000 is MBEDTLS_ERR_X509_CERT_VERIFY_FAILED.) The boot-time
             * detect fires at ~3 s while SNTP typically lands at ~36 s, so waiting
             * here is what makes automatic IP location actually work. Bounded to
             * 75 s so it can never hang the worker; the periodic refresh and the
             * page's "重新定位" button cover the rest.
             *
             * This runs on the weather worker task, not in a caller, so the wait
             * delays nothing that the user is blocking on.
             */
            bool clock_ready = sys_time_synced();
            for (int attempt = 0; attempt < 15 && !clock_ready; attempt++) {
                if (attempt == 0) {
                    SYS_LOGI(TAG, "waiting for the clock before IP geolocation");
                }
                vTaskDelay(pdMS_TO_TICKS(5000));
                clock_ready = sys_time_synced();
            }
            if (!clock_ready) {
                SYS_LOGW(TAG, "clock still not synced after 75 s - skipping IP geolocation");
            } else {
                /* Priority 2 of the FR3 location list: only used when nothing was
                 * chosen manually, or when the caller explicitly asked for it. */
                const esp_err_t geo_err = weather_detect_location();
                if (geo_err != ESP_OK) {
                    SYS_LOGD(TAG, "IP geolocation unavailable (%s)", esp_err_to_name(geo_err));
                }
            }
        }

        const int64_t attempt_started = sys_time_ms();
        const esp_err_t err = weather_do_refresh();
        if (err != ESP_OK) {
            /*
             * 传输层失败时再试一次。
             *
             * 拿到地址的那一刻，网络其实**还没完全可用**：DHCP 带来的 DNS
             * 服务器地址、路由、lwIP 的解析器都是随地址一起到的，比事件晚几十
             * 毫秒到几百毫秒。这时候立刻发请求，DNS 会解析不出来
             * （真机日志："couldn't get hostname ... getaddrinfo() returns 202"），
             * 表现为 ESP_ERR_HTTP_CONNECT —— 但网络其实一直是好的。
             *
             * 所以传输失败先歇一下再试一次（最多一次，别把电台耗在重试上）：
             * 这一下就把「联网了但天气不动」变成了「联网后几秒内天气就到位」。
             *
             * 只有「很快就失败」才重试：连接错误是立刻返回的（DNS 解析不出来、
             * 对端拒绝），而超时意味着这一轮已经耗掉了 12 秒；再试一次会把
             * 同步路径的 20 秒预算（WEATHER_REFRESH_WAIT_MS）撑破，接口会从
             * 「回 200 + 缓存读数」变成「回 503」。那才是真的用户可见变化。
             */
            const int64_t spent = sys_time_ms() - attempt_started;
            if (err == ESP_ERR_HTTP_CONNECT && spent < WEATHER_RETRY_BUDGET_MS &&
                weather_network_up()) {
                SYS_LOGI(TAG, "the first attempt hit a connection error - retrying in %d s",
                         (int)(WEATHER_RETRY_DELAY_MS / 1000));
                vTaskDelay(pdMS_TO_TICKS(WEATHER_RETRY_DELAY_MS));
                const esp_err_t retry = weather_do_refresh();
                if (retry == ESP_OK) {
                    xSemaphoreGive(s_done);
                    continue;
                }
                SYS_LOGW(TAG, "refresh failed: %s", esp_err_to_name(retry));
                (void)sys_event_post_to(WEATHER_EVENT, WEATHER_EVENT_REFRESH_FAILED,
                                        &retry, sizeof(retry));
                xSemaphoreGive(s_done);
                continue;
            }
            SYS_LOGW(TAG, "refresh failed: %s", esp_err_to_name(err));
            (void)sys_event_post_to(WEATHER_EVENT, WEATHER_EVENT_REFRESH_FAILED,
                                    &err, sizeof(err));
        }
        xSemaphoreGive(s_done);
    }
    vTaskDelete(NULL);
}

/**
 * @brief 异步做一次 IP 定位（等价于 weather_detect_location()，但不阻塞调用者）。
 *
 * weather_detect_location() 是阻塞的：一次完整的 HTTPS 请求，超时 12 s。
 * 设备端「重新定位」按钮在 LVGL 回调里调用它会把整个 UI 冻住 12 s
 * （AGENT.md §9.0.1.1 铁律一），所以 UI 必须走这个异步入口。
 *
 * 请求丢给天气 worker 任务（栈 8192，跑的是和刷新一样的 HTTPS 路径），
 * 定位完顺带刷一次读数。结果通过 WEATHER_EVENT_UPDATED 事件回来，
 * 页面收到事件后自己刷新即可。
 *
 * 注意：声明还需要补到 components/weather_service/include/weather_service.h
 * （本轮该头文件不在天气分区的改动白名单内，见 not_done）。
 */
esp_err_t weather_detect_location_async(void)
{
    if (!s_initialized || s_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * 先置标志位再叫醒 worker：队列满的时候（worker 正在跑一轮）标志位
     * 依然留着，请求不会丢。worker 下一轮会把它取走。
     */
    weather_request_detect();
    const uint8_t token = WEATHER_REQ_REFRESH;
    if (xQueueSend(s_requests, &token, 0) != pdTRUE) {
        SYS_LOGD(TAG, "detect queued behind a pending refresh");
    }
    return ESP_OK;
}

esp_err_t weather_refresh_async(void)
{
    if (!s_initialized || s_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t token = WEATHER_REQ_REFRESH;
    if (xQueueSend(s_requests, &token, 0) != pdTRUE) {
        /* A refresh is already queued: coalescing is the desired behaviour. */
        return ESP_OK;
    }
    return ESP_OK;
}

esp_err_t weather_refresh(void)
{
    if (!s_initialized || s_requests == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_done, 0);   /* drop a stale completion */
    const uint8_t token = WEATHER_REQ_REFRESH;
    if (xQueueSend(s_requests, &token, 0) != pdTRUE) {
        /* Someone else is refreshing: wait for that result instead of piling on. */
    }

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(WEATHER_REFRESH_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void refresh_timer_cb(void *arg)
{
    (void)arg;
    (void)weather_refresh_async();
}

/* ------------------------------------------------------------------------ */
/* Configuration / queries                                                   */
/* ------------------------------------------------------------------------ */

esp_err_t weather_get(weather_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);

    return out->valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

const char *weather_get_city(void)
{
    return s_city;
}

esp_err_t weather_set_city(const char *city)
{
    if (city == NULL || city[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_city, city, sizeof(s_city));
    /* Force a geocode on the next refresh. */
    s_data.lat = 0;
    s_data.lon = 0;
    xSemaphoreGive(s_lock);

    esp_err_t err = config_set_str_if_changed(SYS_KEY_CITY, city);
    SYS_LOGI(TAG, "city set to '%s'", city);

    (void)weather_refresh_async();
    return err;
}

uint32_t weather_get_interval_s(void)
{
    return s_interval_s;
}

esp_err_t weather_set_interval_s(uint32_t seconds)
{
    if (seconds < 300) {
        seconds = 300;
    }
    if (seconds > 86400) {
        seconds = 86400;
    }

    s_interval_s = seconds;
    esp_err_t err = sys_config_set_i32(SYS_KEY_WEATHER_INT, (int32_t)seconds);

    if (s_timer != NULL) {
        (void)esp_timer_stop(s_timer);
        (void)esp_timer_start_periodic(s_timer, (uint64_t)seconds * 1000000ULL);
    }
    SYS_LOGI(TAG, "refresh interval set to %u s", (unsigned)seconds);
    return err;
}

esp_err_t weather_set_api_key(const char *key)
{
    if (key == NULL || key[0] == '\0') {
        (void)sys_config_erase(SYS_KEY_WEATHER_KEY);
        return ESP_OK;
    }
    /* Stored only; never logged and never returned by any API. */
    return sys_config_set_str(SYS_KEY_WEATHER_KEY, key);
}

bool weather_has_api_key(void)
{
    char key[80];
    return weather_read_api_key(key, sizeof(key)) == ESP_OK && key[0] != '\0';
}

/* ------------------------------------------------------------------------ */
/* Init                                                                      */
/* ------------------------------------------------------------------------ */

esp_err_t weather_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_done = xSemaphoreCreateBinary();
    s_requests = xQueueCreate(1, sizeof(uint8_t));
    if (s_done == NULL || s_requests == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Default city: Nanchang, Jiangxi.
     *
     * IP geolocation is only priority 2 of the FR3 location list and it is not
     * reliable here: behind a phone/laptop hotspot the public IP resolved to
     * Kachin, Myanmar (25.53, 97.73) rather than the actual location. A fixed
     * default with real coordinates means weather is correct on the very first
     * boot with no network lookup at all, and "重新定位" is still there when the
     * user actually moves.
     *
     * The price of the fixed default: the "no city -> locate by IP" branch
     * below is never taken at boot. The IP geolocation path itself is not
     * dead code any more - weather_detect_location_async() (the device side
     * "重新定位" button) goes through the same worker path.
     */
    (void)sys_config_get_str_or(SYS_KEY_CITY, s_city, sizeof(s_city), "南昌");
    /*
     * Read the coordinates back too.
     *
     * weather_set_location() has always persisted them (SYS_KEY_CITY_LAT/LON) but
     * nothing ever loaded them, so after a reboot the city name survived while the
     * position did not: have_coords was false, the refresh fell back to geocoding
     * by name (weather still came out right) and /api/v1/weather reported lat/lon 0.
     */
    char coord[24];
    if (sys_config_get_str(SYS_KEY_CITY_LAT, coord, sizeof(coord)) == ESP_OK) {
        s_data.lat = strtod(coord, NULL);
    } else {
        s_data.lat = 28.6820;   /* Nanchang */
    }
    if (sys_config_get_str(SYS_KEY_CITY_LON, coord, sizeof(coord)) == ESP_OK) {
        s_data.lon = strtod(coord, NULL);
    } else {
        s_data.lon = 115.8579;  /* Nanchang */
    }
    (void)sys_config_get_str_or(SYS_KEY_WEATHER_PROV, s_provider, sizeof(s_provider),
                               WEATHER_PROVIDER_DEFAULT);
    (void)sys_config_get_str_or(SYS_KEY_GEO_URL, s_geo_url, sizeof(s_geo_url),
                               WEATHER_GEO_ENDPOINT_DEFAULT);
    s_interval_s = (uint32_t)sys_config_get_i32_or(SYS_KEY_WEATHER_INT,
                                                  CONFIG_TODOHELPER_WEATHER_INTERVAL_S);

    s_initialized = true;

    /* Cache first: the page must render immediately, even with no network. */
    if (weather_load_cache() != ESP_OK) {
        SYS_LOGI(TAG, "no cached weather yet");
    }

    if (xTaskCreate(weather_task, "th_weather", WEATHER_TASK_STACK, NULL,
                    WEATHER_TASK_PRIO, &s_task) != pdPASS) {
        SYS_LOGE(TAG, "refresh task creation failed");
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = refresh_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "th_weather",
    };
    if (esp_timer_create(&timer_args, &s_timer) == ESP_OK) {
        (void)esp_timer_start_periodic(s_timer, (uint64_t)s_interval_s * 1000000ULL);
    }

    esp_err_t http_err = weather_service_register_http();
    if (http_err != ESP_OK) {
        SYS_LOGW(TAG, "HTTP routes unavailable: %s", esp_err_to_name(http_err));
    }

    /*
     * 订阅 WiFi 事件：地址刚出现时把「因为没地址而放弃的那次刷新」补上。
     * 没有它的话，开机时 DHCP 慢一点，天气就会一直显示缓存值，
     * 直到 900 秒的周期定时器下一次唤醒它。
     *
     * 订阅在 s_initialized 之后、第一个刷新请求之前做：这样「地址已经在了」
     * 的情况（热启动、或者 WiFi 先连上）走的是 s_had_ip 的初值，不会多刷一次。
     */
    s_had_ip = weather_network_up();
    const esp_err_t wifi_sub = sys_event_subscribe(WIFI_MGR_EVENT, ESP_EVENT_ANY_ID,
                                                   on_wifi_event, NULL, NULL);
    if (wifi_sub != ESP_OK) {
        SYS_LOGW(TAG, "cannot subscribe to the WiFi events: %s",
                 esp_err_to_name(wifi_sub));
    }

    SYS_LOGI(TAG, "ready (provider '%s', city '%s', every %u s, geo endpoint '%s')",
             weather_provider_current()->name, s_city[0] ? s_city : "<unset>",
             (unsigned)s_interval_s, s_geo_url);

    if (s_city[0] != '\0') {
        /* A city is already configured: keep it and just refresh. */
        (void)weather_refresh_async();
    } else {
        /* FR3 location priority 2: with nothing configured, try the public IP so
         * the page is useful on the very first boot without any setup. */
        SYS_LOGI(TAG, "no city configured, trying IP geolocation");
        (void)weather_detect_location_async();
    }
    return ESP_OK;
}
