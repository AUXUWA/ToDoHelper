# bsp

**L1 板级门面** — 把工程外只读的 Korvo BSP 包装成 ToDoHelper 自己的稳定接口，
使业务层不直接依赖 `..\common_components\esp32_s31_korvo`。

- 头文件：`include/th_bsp.h`
- 依赖：`sys_utils`、`esp32_s31_korvo`（只读）、`esp_lvgl_adapter`、`lvgl`
- 符号前缀 **`th_bsp_`**：Korvo BSP 已占用 `bsp_display_*` / `bsp_sdcard_*` / `bsp_led_*`，必须避让

## API 摘要

| 分组 | 函数 |
|---|---|
| 生命周期 | `th_bsp_init()`、`th_bsp_display()` |
| 锁 | `th_bsp_lock()`、`th_bsp_unlock()`（包装 `esp_lv_adapter_lock`，**可重入**） |
| 亮度 | `th_bsp_set_brightness()`、`th_bsp_get_brightness()`、`th_bsp_restore_brightness()` |
| 输入 | `th_bsp_buttons_start()`、`th_bsp_report_activity()`、`th_bsp_idle_ms()`、`th_bsp_is_idle()`、`th_bsp_poll_idle()`、`th_bsp_idle_init/stop()` |
| LED | `th_bsp_led_set()` |
| SD | `th_bsp_sdcard_mount/unmount/mounted/mount_point()` |
| 音频 | `th_bsp_audio_init()`、`th_bsp_audio_speaker()`、`th_bsp_audio_mic()`、`th_bsp_audio_available()` |

## 关键实现说明

1. **显示启动参数与产测基线一致**：800×480、`TEAR_AVOID_MODE_TRIPLE_PARTIAL`、
   LVGL buffer 高 160、显示任务栈 16 KB、PPA 加速开启。业务层**不得**再次调用
   `bsp_display_start_*` 或 `esp_lv_adapter_init`。
2. **亮度是软件调光**：本板 `BSP_LCD_BACKLIGHT = GPIO_NUM_NC`，没有背光 PWM 脚，
   因此用 `lv_layer_top()` 上的黑色遮罩做调光；遮罩**去掉 CLICKABLE**，
   保证变暗后触摸仍然可用。`0%` 是"最暗"而不是全黑，避免用户以为死机。
3. **按键只转发两个逻辑事件**：`BUTTON_SINGLE_CLICK` → `SYS_EVENT_BUTTON`（PRESS），
   `BUTTON_LONG_PRESS_START` → `SYS_EVENT_BUTTON`（LONG_PRESS）。
   `iot_button` 回调运行在按键任务，**不得**在其中操作 LVGL。
4. **待机看门狗**：`esp_timer` 每 500 ms 轮询，超过 NVS `idle_timeout`（默认 30 s）
   发 `SYS_EVENT_IDLE_TIMEOUT`；任意交互发 `SYS_EVENT_WAKEUP`。**策略**在 `ui_core`。
5. **音频惰性初始化**：`th_bsp_audio_init()` 失败不算致命，FR7 走降级路径。

## 测试方法

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd D:\project\ESP-IDFv6.1\ToDoHelper
idf.py build
idf.py -p COM34 flash monitor
```

期望日志：

```
I (xxx) TH:bsp: ready: 800x480, tear-avoid=triple-partial, ppa=1, brightness=100%
I (xxx) TH:bsp_input: 4 ADC buttons registered
I (xxx) TH:bsp_input: idle timeout 30 s
```

- 屏幕应出现深色背景 + 顶部状态栏（属 `ui_core`）。
- 按 MODE 键应在首页与待办页之间切换；30 s 无操作屏幕变暗，触摸后恢复。
