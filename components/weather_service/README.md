# weather_service

**L3 业务组件（FR3）** — 手动选城市的天气：数据全部来自网络 API，本地没有传感器也没有 GPS。

- 头文件：`include/weather_service.h`（provider 接口在私有头 `src/weather_internal.h`）
- 依赖：`sys_utils`、`storage_manager`、`http_router`、`cjson`、IDF `esp_http_client` / `esp-tls` / `mbedtls` / `esp_timer`
- 被谁依赖：`ui_pages`（`page_weather.c`、`page_settings.c`）、`main`

## 为什么"必须联网"

需求 FR3 / §4 明确：**本板没有温度传感器、没有 GPS 模块**。因此

- 温度、体感、湿度、天气现象**一律**取 API 返回值，不存在"本地测量"这条路径；
- 定位优先级里"手动设置城市"是 **P0 唯一可用路径**（IP 定位是 P1，GPS 已判定为硬件不支持）；
- 掉网时不是空白页，而是**显示上一次缓存**（`cache/weather.json`），并由 `stale` 标记提示数据已过期。

## Provider（可插拔）

`weather_provider_t`（`src/weather_internal.h`）只有四个成员：

| 成员 | 含义 |
|---|---|
| `name` | 短名，用于 NVS 键 `weather_prov` 与 HTTP API |
| `needs_api_key` | 为真时刷新前会检查 NVS `weather_key`，缺失则 `ESP_ERR_NOT_SUPPORTED` |
| `geocode(city, &lat, &lon, label, len)` | 城市名 → 经纬度（`label` 可返回 "name, admin1" 形式的显示名） |
| `fetch(lat, lon, out)` | 按坐标取当前读数 |

编译进固件的 provider 只有 `weather_provider_open_meteo`（`weather_provider_names()` 可枚举）。
**默认的 Open-Meteo 完全不需要 API Key**（`needs_api_key = false`），所以 FR3 开箱即用；
NVS 键 `weather_key` 是给和风 / OpenWeather 这类付费 provider 预留的，接口已经就位但 provider 尚未实现。
选择未知 provider 返回 `ESP_ERR_NOT_SUPPORTED`。

默认 provider 的两个端点：`geocoding-api.open-meteo.com/v1/search?name=<percent-encoded>&count=1&language=zh&format=json`
与 `api.open-meteo.com/v1/forecast?...&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code&timezone=auto`。
城市名按 UTF-8 **逐字节 percent-encode**，所以中文城市名可用。

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `weather_service_init()`、`weather_refresh()`、`weather_refresh_async()` |
| 查询 | `weather_get()`、`weather_get_city()`、`weather_get_provider()`、`weather_get_interval_s()` |
| 配置 | `weather_set_city()`、`weather_set_provider()`、`weather_set_interval_s()`、`weather_set_api_key()`、`weather_has_api_key()` |
| 缓存 | `weather_load_cache()`、`weather_save_cache()` |
| HTTP | `weather_service_register_http()`（由 `init()` 自动调用） |

## 关键约定

1. **HTTP 请求在 worker 任务里跑，不在 LVGL 任务里**。`weather_service_init()` 创建 `th_weather`
   任务（栈 6144、优先级 4）并配一个深度为 1 的请求队列；`weather_refresh_async()` 只是往队列丢一个 token，
   所以从 UI 回调调用也不会卡住 LVGL。队列满 = 已有刷新在排队，直接返回 `ESP_OK`（**合并**多次请求是期望行为）。
   `weather_refresh()`（同步版）丢 token 后用二值信号量等最多 20 s，超时返回 `ESP_ERR_TIMEOUT`。
2. **15 分钟定时刷新**。`esp_timer` 周期计时器（`ESP_TIMER_TASK` 派发）默认周期
   `WEATHER_DEFAULT_INTERVAL_S = 900`，来自 NVS `weather_int` / `CONFIG_TODOHELPER_WEATHER_INTERVAL_S`；
   `weather_set_interval_s()` 会把值夹到 **300–86400** 秒并立刻重启计时器。开机时若已存有城市会主动刷一次。
3. **缓存与 stale**。`weather_save_cache()` 写 `cache/weather.json`（字段：`city, lat, lon, temp, feels_like,
   humidity, condition, icon, weather_code, update_ts, provider`），**绝不写入 API Key**（需求 §8.8）。
   `weather_load_cache()` 在 `init()` 里先于一切执行，只要 `city` 或 `condition` 非空就认为 `valid = true`，
   页面可以立刻渲染。HTTP 里 `stale` 的判定是
   `now > 0 && update_ts > 0 && (now - update_ts) > WEATHER_STALE_AFTER_S`，而
   `WEATHER_STALE_AFTER_S = 2 × 900 = 1800` 秒；**时钟未同步时 `now == 0`，`stale` 恒为 false**（无法判断，不误报）。
4. **刷新失败不是致命错误**。离线（`sys_status_get()` 里 `!online && !wifi_connected`）直接返回
   `ESP_ERR_INVALID_STATE`；未设城市返回 `ESP_ERR_NOT_FOUND`；失败发
   `WEATHER_EVENT_REFRESH_FAILED`（payload `esp_err_t`），成功发 `WEATHER_EVENT_UPDATED`（无 payload）
   并 `sys_notify("weather_changed")` 给 SSE。缓存里的旧读数始终保留可见。
5. **WMO 天气码 → 中文描述 + 图标名**（`weather_wmo_to_condition()`）。映射覆盖
   `0 晴 / 1 少云 / 2 多云 / 3 阴 / 45,48 雾 / 51,53,55 毛毛雨 / 56,57 冻雨 / 61,63,65 小-大 雨 /
   66,67 冻雨 / 71,73,75 小-大 雪 / 77 米雪 / 80,81,82 阵雨-暴雨 / 85,86 阵雪 / 95 雷阵雨 / 96,99 雷阵雨伴冰雹`，
   未知码回落到 **`未知` + `w_overcast`**。
   图标名是 `ui_assets` 的**基名**（打包进 `assets` 分区的 `w_*.png`，实际用到
   `w_sunny / w_cloudy / w_overcast / w_fog / w_rain / w_snow / w_thunder`）。
6. **API Key 只进不出**。`weather_set_api_key()` 只写 NVS 键 `weather_key`（**不写日志**，不返回）；
   `POST /api/v1/weather/config` 的响应只给 `has_api_key: true|false`，
   `weather_has_api_key()` 是唯一的对外查询方式。传空字符串会 `sys_config_erase()` 清除。
7. **HTTPS 用证书包**（`esp_crt_bundle_attach`），不做 leaf pinning；响应体上限 16 KB，要求 HTTP 200，
   非 200 记 `HTTP <code> for <url>` 并返回 `ESP_ERR_INVALID_RESPONSE`。超时由
   `CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS`（默认 12000 ms）控制。
8. **城市是唯一输入**。`weather_set_city()` 会清空已缓存的经纬度以强制重新 geocode，把城市写进 NVS `city`，
   然后异步刷新；刷新成功后把解析到的真实地名存回 `city`、把坐标以字符串存进 `city_lat` / `city_lon`
   （仅作诊断，刷新时若 `s_data.city == s_city` 且坐标非零则跳过 geocode，省一次请求）。

## HTTP 路由表（需求 §8.4）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/weather` | 当前读数；无读数时仍返回 **200** 但 `valid:false`，让页面提示"请先选城市" |
| POST | `/api/v1/weather/refresh` | 同步刷新；未设城市 → 400，离线/超时 → 503 |
| POST | `/api/v1/weather/city` | body `{"city":"北京"}`，持久化后立即刷新一次 |
| POST | `/api/v1/weather/config` | body 可含 `provider` / `api_key` / `interval_s`，响应只回 `provider`、`has_api_key`、`interval_s` |

`update_ts` 是读数的时间戳（Unix 秒，时钟未同步时可能为 0），`server_ts` 是响应时刻的设备时间。

## Kconfig

- `CONFIG_TODOHELPER_WEATHER_INTERVAL_S`（默认 900，范围 300–86400）— 刷新周期，NVS 键 `weather_int` 可覆盖。
- `CONFIG_TODOHELPER_WEATHER_HTTP_TIMEOUT_MS`（默认 12000，范围 3000–60000）— 单次 HTTP 超时。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

首次上电（未设城市、无缓存）：

```
I (xxx) TH:weather: no cached weather yet
I (xxx) TH:weather: ready (provider 'open-meteo', city '<unset>', every 900 s)
```

设置城市后（**不需要任何 API Key**）：

```powershell
curl.exe -X POST http://todohelper.local/api/v1/weather/city -H "Content-Type: application/json" -d '{\"city\":\"Beijing\"}'
curl.exe http://todohelper.local/api/v1/weather
```

期望串口输出（坐标/温度随城市与实时天气变化，此处仅示例）：

```
I (xxx) TH:weather: city set to 'Beijing'
I (xxx) TH:weather_om: 'Beijing' -> 39.9075, 116.3972
I (xxx) TH:weather_om: 4.0 C, feels 2.1 C, 46%, '晴' (code 0)
```

断网验证：拔掉/关闭 AP 后 `POST /api/v1/weather/refresh` 应返回 503
`"offline, showing the cached reading"`，串口出现
`I (xxx) TH:weather: refresh failed: ESP_ERR_INVALID_STATE`，而 `GET /api/v1/weather` 仍返回上次读数。
重启后应看到 `I (xxx) TH:weather: cache loaded: Beijing 4.0 C '晴'`。
