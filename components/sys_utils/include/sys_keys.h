/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sys_keys.h
 * @brief Every NVS key name used in the project.
 *
 * Components must use these macros instead of raw string literals so that key
 * names stay in one place. Adding a key means: add it here, register it in
 * docs/API.md, and add a row to docs/TODOHELPER_REQUIREMENTS.md 9.1.
 *
 * NOTE: NVS limits key names to 15 characters (NVS_KEY_NAME_MAX_SIZE - 1).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_TODOHELPER_NVS_NAMESPACE
#define CONFIG_TODOHELPER_NVS_NAMESPACE "todohelper"
#endif

/** NVS namespace shared by the whole project. */
#define SYS_NVS_NAMESPACE      CONFIG_TODOHELPER_NVS_NAMESPACE

/* ---- WiFi (FR1) ---- */
#define SYS_KEY_WIFI_SSID      "wifi_ssid"
#define SYS_KEY_WIFI_PASS      "wifi_pass"
#define SYS_KEY_WIFI_AUTO      "wifi_auto"
/* Optional static address for the station: empty = DHCP. */
#define SYS_KEY_WIFI_IP        "wifi_ip"
/* The gateway / netmask of the last DHCP lease, so a static address can use
 * the same network parameters without asking the user for all three. */
#define SYS_KEY_WIFI_GW        "wifi_gw"
#define SYS_KEY_WIFI_MASK      "wifi_mask"

/* ---- Locale / time (FR3, FR4) ---- */
#define SYS_KEY_TZ             "tz"
#define SYS_KEY_CITY           "city"
#define SYS_KEY_CITY_LAT       "city_lat"
#define SYS_KEY_CITY_LON       "city_lon"
#define SYS_KEY_NTP_SERVER     "ntp_server"
#define SYS_KEY_WEATHER_KEY    "weather_key"
#define SYS_KEY_WEATHER_PROV   "weather_prov"
#define SYS_KEY_WEATHER_INT    "weather_int"
#define SYS_KEY_GEO_URL        "geo_url"

/* ---- Device / UI ---- */
#define SYS_KEY_BRIGHTNESS     "brightness"
#define SYS_KEY_IDLE_TIMEOUT   "idle_timeout"
#define SYS_KEY_IDLE_BRIGHT    "idle_bright"
/**
 * One-shot migration marker for the idle-brightness default change (v1.2.0).
 *
 * The default went 20 -> 100 so the idle eyes stay pure white. A previously saved
 * value would win over the new default, so app_main clears the old key once and
 * sets this flag; the settings slider writes `idle_bright` again if the user wants
 * dimming. Remove together with the migration block in main/app_main.c.
 */
#define SYS_KEY_BRIGHT_MIGRATED "brt_migr"
#define SYS_KEY_FIRST_BOOT     "first_boot"
#define SYS_KEY_LANGUAGE       "language"

/* ---- Words (FR5) ---- */
#define SYS_KEY_WORD_DAILY     "word_daily"
#define SYS_KEY_WORD_BOOK      "word_book"

/* ---- MaiMemo OpenAPI (FR5, plans A/B/C) ----
 *
 * `SYS_KEY_MAIMEMO_TOKEN` is a bearer credential: it is written through
 * sys_config_set_str(), never logged and never returned by any HTTP route
 * (the same rule SYS_KEY_AI_API_KEY follows, requirement 8.8).
 *   - MODE selects where the study queue comes from: "cloud" (MaiMemo study
 *     API) or "local" (the book on the SD card).
 *   - PUSH enables plan C, sending words the user marks unknown back to the
 *     MaiMemo study plan. Off by default because it mutates cloud data. */
#define SYS_KEY_MAIMEMO_TOKEN  "mm_token"
#define SYS_KEY_MAIMEMO_MODE   "mm_mode"
#define SYS_KEY_MAIMEMO_PUSH   "mm_push"

/* ---- Sync / HTTP (FR2, 8.8) ---- */
#define SYS_KEY_SYNC_ENABLED   "sync_enabled"
#define SYS_KEY_AUTH_TOKEN     "auth_token"
#define SYS_KEY_HTTP_PORT      "http_port"
#define SYS_KEY_MDNS_ENABLED   "mdns_enabled"

/* ---- AI (FR7, P2) ---- */
#define SYS_KEY_AI_API_KEY     "ai_api_key"
#define SYS_KEY_AI_MODEL       "ai_model"
#define SYS_KEY_AI_PROMPT      "ai_prompt"
#define SYS_KEY_AI_ROUNDS      "ai_rounds"
#define SYS_KEY_AI_SAVE_HIST   "ai_save_hist"
#define SYS_KEY_VOICE_WAKE     "voice_wake"
#define SYS_KEY_TTS_ENABLED    "tts_enabled"
#define SYS_KEY_ASR_URL        "asr_url"
#define SYS_KEY_TTS_URL        "tts_url"

#ifdef __cplusplus
}
#endif
