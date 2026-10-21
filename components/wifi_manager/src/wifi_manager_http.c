/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi_manager_http.c
 * @brief Requirement 8.3: the /api/v1/wifi/... endpoints.
 *
 * Same pattern as the other business components: register on the L1 router so
 * this component never depends on sync_server.
 *
 * | Method | Path | Notes |
 * |---|---|---|
 * | GET  | /api/v1/wifi/status     | state, ssid, ip, rssi, mac, hostname, url |
 * | POST | /api/v1/wifi/scan       | starts an asynchronous scan |
 * | GET  | /api/v1/wifi/scan       | cached results of the last completed scan |
 * | POST | /api/v1/wifi/connect    | body: {"ssid":...,"password":...} |
 * | POST | /api/v1/wifi/disconnect | |
 * | POST | /api/v1/wifi/config     | body: {"ssid","password","auto_connect"} |
 * | POST | /api/v1/wifi/forget     | clears the stored credentials |
 *
 * The scan is always started asynchronously: the blocking variant would tie up
 * the httpd worker for up to 15 s.
 */

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "http_router.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_http";

static char *status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    wifi_mgr_state_t state = WIFI_MGR_DISCONNECTED;
    (void)wifi_manager_get_state(&state);

    char ip[16] = { 0 };
    char mac[18] = { 0 };
    char ssid[WIFI_MANAGER_SSID_MAX + 1] = { 0 };
    (void)wifi_manager_get_ip(ip, sizeof(ip));
    (void)wifi_manager_get_mac(mac, sizeof(mac));
    (void)wifi_manager_get_ssid(ssid, sizeof(ssid));

    cJSON_AddStringToObject(root, "state", wifi_manager_state_name(state));
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddStringToObject(root, "hostname", WIFI_MANAGER_HOSTNAME);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ------------------------------------------------------------------------ */
/* Handlers                                                                  */
/* ------------------------------------------------------------------------ */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *json = status_json();
    if (json == NULL) {
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "wifi status");
    }
    const esp_err_t err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t scan_post_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_manager_start_scan();
    if (err != ESP_OK) {
        return http_reply_unavailable(req, esp_err_to_name(err));
    }
    return http_reply_ok(req, "{\"scanning\":true}");
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    wifi_ap_info_t aps[WIFI_MANAGER_MAX_APS];
    size_t count = 0;
    esp_err_t err = wifi_manager_get_scan_result(aps, WIFI_MANAGER_MAX_APS, &count);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "wifi scan");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (root == NULL || array == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(array);
        return http_reply_esp_error(req, ESP_ERR_NO_MEM, "wifi scan");
    }
    cJSON_AddItemToObject(root, "items", array);

    const size_t shown = (count > WIFI_MANAGER_MAX_APS) ? WIFI_MANAGER_MAX_APS : count;
    for (size_t i = 0; i < shown; i++) {
        cJSON *ap = cJSON_CreateObject();
        if (ap == NULL) {
            continue;
        }
        cJSON_AddStringToObject(ap, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(ap, "channel", aps[i].channel);
        cJSON_AddNumberToObject(ap, "authmode", aps[i].authmode);
        cJSON_AddBoolToObject(ap, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(array, ap);
    }
    cJSON_AddNumberToObject(root, "count", (double)count);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    err = http_reply_ok(req, json);
    cJSON_free(json);
    return err;
}

/** Read {"ssid":...,"password":...,"save":bool} from the body. */
static esp_err_t read_credentials(httpd_req_t *req, char *ssid, size_t ssid_len,
                                  char *password, size_t pass_len, bool *save)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(ssid, ssid_item->valuestring, ssid_len);

    const cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    password[0] = '\0';
    if (cJSON_IsString(pass_item)) {
        if (strlen(pass_item->valuestring) >= pass_len) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_SIZE;
        }
        strlcpy(password, pass_item->valuestring, pass_len);
    }

    if (save != NULL) {
        const cJSON *save_item = cJSON_GetObjectItemCaseSensitive(root, "save");
        *save = cJSON_IsBool(save_item) ? cJSON_IsTrue(save_item) : true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req)
{
    char ssid[WIFI_MANAGER_SSID_MAX + 1];
    char password[WIFI_MANAGER_PASS_MAX + 1];

    esp_err_t err = read_credentials(req, ssid, sizeof(ssid), password, sizeof(password), NULL);
    if (err == ESP_ERR_INVALID_SIZE) {
        return http_reply_bad_request(req, "password too long");
    }
    if (err != ESP_OK) {
        return http_reply_bad_request(req, "field 'ssid' is required");
    }

    /* Credentials are persisted on success of the attempt, matching FR1's
     * "remember the network" behaviour. */
    err = wifi_manager_connect(ssid, password);
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "connect");
    }

    char data[128];
    snprintf(data, sizeof(data), "{\"connecting\":true,\"ssid\":\"%s\"}", ssid);
    return http_reply_ok(req, data);
}

static esp_err_t disconnect_post_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_manager_disconnect();
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "disconnect");
    }
    return http_reply_ok(req, "{\"disconnected\":true}");
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char *body = NULL;
    if (http_read_body(req, &body, NULL) != ESP_OK) {
        return http_reply_bad_request(req, "missing body");
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return http_reply_bad_request(req, "invalid json");
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    const cJSON *auto_connect = cJSON_GetObjectItemCaseSensitive(root, "auto_connect");
    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(root, "ip");

    esp_err_t err = ESP_OK;
    if (cJSON_IsString(ssid)) {
        err = wifi_manager_save_config(ssid->valuestring,
                                      cJSON_IsString(password) ? password->valuestring : "");
    }
    if (cJSON_IsBool(auto_connect) && err == ESP_OK) {
        err = sys_config_set_bool(SYS_KEY_WIFI_AUTO, cJSON_IsTrue(auto_connect));
    }
    if (cJSON_IsString(ip) && err == ESP_OK) {
        err = wifi_manager_set_static_ip(ip->valuestring);
    }
    cJSON_Delete(root);

    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "wifi config");
    }

    char data[64];
    snprintf(data, sizeof(data), "{\"auto_connect\":%s}",
             sys_config_get_bool_or(SYS_KEY_WIFI_AUTO, true) ? "true" : "false");
    return http_reply_ok(req, data);
}

static esp_err_t forget_post_handler(httpd_req_t *req)
{
    esp_err_t err = wifi_manager_clear_config();
    if (err != ESP_OK) {
        return http_reply_esp_error(req, err, "forget");
    }
    return http_reply_ok(req, "{\"cleared\":true}");
}

/* ------------------------------------------------------------------------ */
/* Registration                                                              */
/* ------------------------------------------------------------------------ */

esp_err_t wifi_manager_register_http(void)
{
    static const struct {
        const char *path;
        httpd_method_t method;
        http_route_handler_t handler;
    } routes[] = {
        { "/api/v1/wifi/status", HTTP_GET, status_get_handler },
        { "/api/v1/wifi/scan", HTTP_GET, scan_get_handler },
        { "/api/v1/wifi/scan", HTTP_POST, scan_post_handler },
        { "/api/v1/wifi/connect", HTTP_POST, connect_post_handler },
        { "/api/v1/wifi/disconnect", HTTP_POST, disconnect_post_handler },
        { "/api/v1/wifi/config", HTTP_POST, config_post_handler },
        { "/api/v1/wifi/forget", HTTP_POST, forget_post_handler },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = http_router_register(routes[i].path, routes[i].method, routes[i].handler);
        if (err != ESP_OK) {
            SYS_LOGE(TAG, "register %s failed: %s", routes[i].path, esp_err_to_name(err));
            return err;
        }
    }
    SYS_LOGD(TAG, "%u wifi routes registered", (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}
