# Factory Demo

This example is the factory-style multimedia validation firmware for the ESP32-S31-Korvo development board.

## Features

* RGB display and GT1151 touch validation with the coffee-machine LVGL demo UI.
* OV3660 or SC101 DVP camera preview rendered through the LVGL canvas.
* Audio record/playback validation through the onboard codec path.
* Board button, WS2812 status LED, and USB HID host validation.
* SD card (SDMMC 4-bit) validation with file read/write probe.

## Notes

* The coffee demo image assets are packed into the `coffee_pjpg` partition and loaded through `esp_mmap_assets`.

## Build and Flash

```bash
cd examples/esp32-s31-korvo/examples/factory_demo
idf.py --preview set-target esp32s31
idf.py build
idf.py flash monitor
```
