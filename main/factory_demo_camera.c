/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "factory_demo_camera.h"

#include <inttypes.h>
#include <string.h>
#include "bsp/esp32_s31_korvo.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#define FACTORY_CAMERA_PREVIEW_WIDTH      240
#define FACTORY_CAMERA_PREVIEW_HEIGHT     240
#define FACTORY_CAMERA_TASK_STACK         8192
#define FACTORY_CAMERA_TASK_PRIORITY      6
#define FACTORY_CAMERA_PPA_ALIGNMENT      64
#define FACTORY_CAMERA_PPA_SCALE_STEPS    1024U

static const char *TAG = "factory_camera";

typedef enum {
    FACTORY_CAMERA_PIPELINE_UNKNOWN = 0,
    FACTORY_CAMERA_PIPELINE_RGB565_DIRECT,
    FACTORY_CAMERA_PIPELINE_RGB565_SCALED,
    FACTORY_CAMERA_PIPELINE_YUV422_UYVY,
} factory_camera_pipeline_t;

static void factory_camera_set_status_locked(factory_camera_t *camera, factory_camera_state_t state, const char *status)
{
    camera->state = state;
    strlcpy(camera->status, status, sizeof(camera->status));
}

static void factory_camera_set_status(factory_camera_t *camera, factory_camera_state_t state, const char *status)
{
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    factory_camera_set_status_locked(camera, state, status);
    xSemaphoreGive(camera->lock);
}

static esp_err_t factory_camera_register_ppa(factory_camera_t *camera)
{
    if (camera->ppa_srm) {
        return ESP_OK;
    }

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &camera->ppa_srm), TAG, "register PPA SRM client failed");
    ESP_LOGI(TAG, "PPA SRM client registered");
    return ESP_OK;
}

static size_t factory_camera_row_bytes(uint32_t width)
{
    return (size_t)width * sizeof(uint16_t);
}

static size_t factory_camera_frame_bytes(uint32_t width, uint32_t height)
{
    return factory_camera_row_bytes(width) * height;
}

static uint32_t factory_camera_get_stride(const bsp_camera_format_t *format)
{
    return format->bytesperline ? format->bytesperline : (uint32_t)factory_camera_row_bytes(format->width);
}

static bool factory_camera_is_rgb565(uint32_t pixelformat)
{
    return pixelformat == V4L2_PIX_FMT_RGB565X || pixelformat == V4L2_PIX_FMT_RGB565;
}

static bool factory_camera_needs_byte_swap(uint32_t pixelformat)
{
    return pixelformat == V4L2_PIX_FMT_RGB565X;
}

static factory_camera_pipeline_t factory_camera_select_pipeline(const bsp_camera_format_t *format)
{
    if (!format) {
        return FACTORY_CAMERA_PIPELINE_UNKNOWN;
    }
    if (factory_camera_is_rgb565(format->pixelformat)) {
        if (format->width == FACTORY_CAMERA_PREVIEW_WIDTH && format->height == FACTORY_CAMERA_PREVIEW_HEIGHT) {
            return FACTORY_CAMERA_PIPELINE_RGB565_DIRECT;
        }
        return FACTORY_CAMERA_PIPELINE_RGB565_SCALED;
    }
    if (format->pixelformat == V4L2_PIX_FMT_UYVY) {
        return FACTORY_CAMERA_PIPELINE_YUV422_UYVY;
    }
    return FACTORY_CAMERA_PIPELINE_UNKNOWN;
}

static const char *factory_camera_pipeline_name(factory_camera_pipeline_t pipeline)
{
    switch (pipeline) {
    case FACTORY_CAMERA_PIPELINE_RGB565_DIRECT:
        return "RGB565 direct";
    case FACTORY_CAMERA_PIPELINE_RGB565_SCALED:
        return "RGB565 PPA";
    case FACTORY_CAMERA_PIPELINE_YUV422_UYVY:
        return "UYVY PPA";
    default:
        return "unsupported";
    }
}

static bool factory_camera_canvas_supports_direct_ppa(uint32_t canvas_stride)
{
    return canvas_stride == factory_camera_row_bytes(FACTORY_CAMERA_PREVIEW_WIDTH);
}

static esp_err_t factory_camera_alloc_buffers(factory_camera_t *camera,
                                              factory_camera_pipeline_t pipeline,
                                              const bsp_camera_format_t *format)
{
    size_t source_size = 0;
    size_t preview_size = 0;
    const uint32_t packed_stride = (uint32_t)factory_camera_row_bytes(format->width);

    if (pipeline == FACTORY_CAMERA_PIPELINE_RGB565_SCALED &&
            (factory_camera_get_stride(format) != packed_stride || factory_camera_needs_byte_swap(format->pixelformat))) {
        source_size = factory_camera_frame_bytes(format->width, format->height);
    } else if (pipeline == FACTORY_CAMERA_PIPELINE_YUV422_UYVY &&
               factory_camera_get_stride(format) != packed_stride) {
        source_size = factory_camera_frame_bytes(format->width, format->height);
    }
    if (pipeline != FACTORY_CAMERA_PIPELINE_RGB565_DIRECT &&
            !factory_camera_canvas_supports_direct_ppa(camera->canvas_stride)) {
        preview_size = factory_camera_frame_bytes(FACTORY_CAMERA_PREVIEW_WIDTH,
                                                  FACTORY_CAMERA_PREVIEW_HEIGHT);
    }

    if (source_size && !camera->source_buf) {
        camera->source_buf = heap_caps_aligned_calloc(FACTORY_CAMERA_PPA_ALIGNMENT, 1, source_size,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(camera->source_buf, ESP_ERR_NO_MEM, TAG, "alloc camera source buffer failed");
    }
    if (preview_size && !camera->preview_buf) {
        camera->preview_buf = heap_caps_aligned_calloc(FACTORY_CAMERA_PPA_ALIGNMENT, 1, preview_size,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(camera->preview_buf, ESP_ERR_NO_MEM, TAG, "alloc preview PPA buffer failed");
    }
    return ESP_OK;
}

static void factory_camera_free_buffers(factory_camera_t *camera)
{
    if (camera->source_buf) {
        heap_caps_free(camera->source_buf);
        camera->source_buf = NULL;
    }
    if (camera->preview_buf) {
        heap_caps_free(camera->preview_buf);
        camera->preview_buf = NULL;
    }
}

static esp_err_t factory_camera_sync_source_for_ppa(void *buffer, size_t size)
{
    ESP_RETURN_ON_FALSE(buffer && size, ESP_ERR_INVALID_ARG, TAG, "invalid PPA source buffer");
    return esp_cache_msync(buffer, size,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                           ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static inline void factory_camera_write_rgb565_pixel(uint8_t *dst, const uint8_t *src, bool swap_bytes);

static void factory_camera_copy_preview_to_canvas(uint8_t *dst, uint32_t dst_stride,
                                                  const uint8_t *src, bool swap_bytes)
{
    const size_t row_bytes = FACTORY_CAMERA_PREVIEW_WIDTH * sizeof(uint16_t);

    for (uint32_t y = 0; y < FACTORY_CAMERA_PREVIEW_HEIGHT; y++) {
        uint8_t *dst_row = dst + y * dst_stride;
        const uint8_t *src_row = src + y * row_bytes;

        if (!swap_bytes) {
            memcpy(dst_row, src_row, row_bytes);
            continue;
        }

        for (uint32_t x = 0; x < FACTORY_CAMERA_PREVIEW_WIDTH; x++) {
            const uint8_t *src_pixel = src_row + x * sizeof(uint16_t);
            uint8_t *dst_pixel = dst_row + x * sizeof(uint16_t);
            factory_camera_write_rgb565_pixel(dst_pixel, src_pixel, true);
        }
    }
}

static inline void factory_camera_write_rgb565_pixel(uint8_t *dst, const uint8_t *src, bool swap_bytes)
{
    if (swap_bytes) {
        dst[0] = src[1];
        dst[1] = src[0];
    } else {
        dst[0] = src[0];
        dst[1] = src[1];
    }
}

static void factory_camera_pack_rgb565_to_native(uint8_t *dst, const uint8_t *src,
                                                 uint32_t src_stride, uint32_t width,
                                                 uint32_t height, bool swap_bytes)
{
    const size_t row_bytes = factory_camera_row_bytes(width);

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dst_row = dst + y * row_bytes;
        const uint8_t *src_row = src + y * src_stride;

        if (!swap_bytes) {
            memcpy(dst_row, src_row, row_bytes);
            continue;
        }
        for (uint32_t x = 0; x < width; x++) {
            dst_row[x * sizeof(uint16_t)] = src_row[x * sizeof(uint16_t) + 1];
            dst_row[x * sizeof(uint16_t) + 1] = src_row[x * sizeof(uint16_t)];
        }
    }
}

static void factory_camera_pack_rows(uint8_t *dst, uint32_t dst_stride,
                                     const uint8_t *src, uint32_t src_stride,
                                     uint32_t row_bytes, uint32_t height)
{
    for (uint32_t y = 0; y < height; y++) {
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
    }
}

static bool factory_camera_frame_matches_preview(const bsp_camera_format_t *format)
{
    return format &&
           format->width == FACTORY_CAMERA_PREVIEW_WIDTH &&
           format->height == FACTORY_CAMERA_PREVIEW_HEIGHT;
}

static void factory_camera_copy_direct_preview_to_canvas(uint8_t *dst, uint32_t dst_stride,
                                                          const uint8_t *src, uint32_t src_stride,
                                                          bool swap_bytes)
{
    for (uint32_t y = 0; y < FACTORY_CAMERA_PREVIEW_HEIGHT; y++) {
        uint8_t *dst_row = dst + y * dst_stride;
        const uint8_t *src_row = src + y * src_stride;

        for (uint32_t x = 0; x < FACTORY_CAMERA_PREVIEW_WIDTH; x++) {
            const uint8_t *src_pixel = src_row + x * sizeof(uint16_t);
            uint8_t *dst_pixel = dst_row + x * sizeof(uint16_t);
            factory_camera_write_rgb565_pixel(dst_pixel, src_pixel, swap_bytes);
        }
    }
}

static esp_err_t factory_camera_ppa_srm(factory_camera_t *camera,
                                        const void *src,
                                        uint32_t src_w,
                                        uint32_t src_h,
                                        uint32_t block_w,
                                        uint32_t block_h,
                                        uint32_t block_offset_x,
                                        uint32_t block_offset_y,
                                        void *dst,
                                        size_t dst_size,
                                        uint32_t dst_w,
                                        uint32_t dst_h,
                                        uint32_t dst_offset_x,
                                        uint32_t dst_offset_y,
                                        float scale_x,
                                        float scale_y,
                                        ppa_srm_color_mode_t input_color_mode)
{
    ppa_srm_oper_config_t srm_config = {
        .in = {
            .buffer = src,
            .pic_w = src_w,
            .pic_h = src_h,
            .block_w = block_w,
            .block_h = block_h,
            .block_offset_x = block_offset_x,
            .block_offset_y = block_offset_y,
            .srm_cm = input_color_mode,
            .yuv_range = PPA_COLOR_RANGE_FULL,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = dst,
            .buffer_size = dst_size,
            .pic_w = dst_w,
            .pic_h = dst_h,
            .block_offset_x = dst_offset_x,
            .block_offset_y = dst_offset_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale_x,
        .scale_y = scale_y,
        .byte_swap = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_scale_rotate_mirror(camera->ppa_srm, &srm_config);
}

static float factory_camera_quantized_scale(uint32_t src_size, uint32_t dst_size)
{
    const uint32_t steps = (uint32_t)(((uint64_t)dst_size * FACTORY_CAMERA_PPA_SCALE_STEPS +
                                       src_size - 1U) / src_size);
    return (float)steps / (float)FACTORY_CAMERA_PPA_SCALE_STEPS;
}

static esp_err_t factory_camera_prepare_ppa_source(factory_camera_t *camera,
                                                   factory_camera_pipeline_t pipeline,
                                                   const bsp_camera_format_t *format,
                                                   const bsp_camera_frame_t *frame,
                                                   const uint8_t **out_src)
{
    const uint32_t packed_stride = (uint32_t)factory_camera_row_bytes(format->width);
    const uint32_t src_stride = factory_camera_get_stride(format);
    const size_t packed_size = factory_camera_frame_bytes(format->width, format->height);

    if (pipeline == FACTORY_CAMERA_PIPELINE_YUV422_UYVY) {
        if (src_stride == packed_stride) {
            *out_src = (const uint8_t *)frame->data;
            return ESP_OK;
        }
        ESP_RETURN_ON_FALSE(camera->source_buf, ESP_ERR_NO_MEM, TAG, "packed UYVY buffer is unavailable");
        factory_camera_pack_rows((uint8_t *)camera->source_buf, packed_stride,
                                 (const uint8_t *)frame->data, src_stride,
                                 packed_stride, format->height);
    } else {
        const bool swap_bytes = factory_camera_needs_byte_swap(format->pixelformat);
        if (src_stride == packed_stride && !swap_bytes) {
            *out_src = (const uint8_t *)frame->data;
            return ESP_OK;
        }
        ESP_RETURN_ON_FALSE(camera->source_buf, ESP_ERR_NO_MEM, TAG, "native RGB565 buffer is unavailable");
        factory_camera_pack_rgb565_to_native((uint8_t *)camera->source_buf,
                                             (const uint8_t *)frame->data,
                                             src_stride, format->width, format->height,
                                             swap_bytes);
    }

    ESP_RETURN_ON_ERROR(factory_camera_sync_source_for_ppa(camera->source_buf, packed_size),
                        TAG, "sync camera source buffer failed");
    *out_src = (const uint8_t *)camera->source_buf;
    return ESP_OK;
}

static void factory_camera_render_preview(factory_camera_t *camera,
                                          factory_camera_pipeline_t pipeline,
                                          const bsp_camera_format_t *format,
                                          const bsp_camera_frame_t *frame,
                                          lv_obj_t *canvas,
                                          void *canvas_buf,
                                          uint32_t canvas_stride)
{
    const size_t preview_size = factory_camera_frame_bytes(FACTORY_CAMERA_PREVIEW_WIDTH,
                                                            FACTORY_CAMERA_PREVIEW_HEIGHT);
    const uint32_t crop_size = format->width < format->height ? format->width : format->height;
    const uint32_t src_offset_x = (format->width - crop_size) / 2;
    const uint32_t src_offset_y = (format->height - crop_size) / 2;
    const ppa_srm_color_mode_t input_color_mode = pipeline == FACTORY_CAMERA_PIPELINE_YUV422_UYVY ?
                                                  PPA_SRM_COLOR_MODE_YUV422_UYVY :
                                                  PPA_SRM_COLOR_MODE_RGB565;
    const uint8_t *src = NULL;
    esp_err_t ret = factory_camera_prepare_ppa_source(camera, pipeline, format, frame, &src);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Prepare PPA camera source failed (%s)", esp_err_to_name(ret));
        return;
    }

    if (factory_camera_canvas_supports_direct_ppa(canvas_stride)) {
        if (bsp_display_lock(-1)) {
            ret = factory_camera_ppa_srm(camera, src, format->width, format->height,
                                         crop_size, crop_size,
                                         src_offset_x, src_offset_y,
                                         canvas_buf,
                                         canvas_stride * FACTORY_CAMERA_PREVIEW_HEIGHT,
                                         FACTORY_CAMERA_PREVIEW_WIDTH,
                                         FACTORY_CAMERA_PREVIEW_HEIGHT,
                                         0, 0,
                                         factory_camera_quantized_scale(crop_size, FACTORY_CAMERA_PREVIEW_WIDTH),
                                         factory_camera_quantized_scale(crop_size, FACTORY_CAMERA_PREVIEW_HEIGHT),
                                         input_color_mode);
            if (ret == ESP_OK) {
                lv_obj_invalidate(canvas);
            } else {
                ESP_LOGW(TAG, "PPA preview direct canvas scale failed (%s)", esp_err_to_name(ret));
            }
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "Skip camera preview because LVGL lock is unavailable");
        }
        return;
    }

    ret = factory_camera_ppa_srm(camera, src, format->width, format->height,
                                 crop_size, crop_size,
                                 src_offset_x, src_offset_y,
                                 camera->preview_buf, preview_size,
                                 FACTORY_CAMERA_PREVIEW_WIDTH,
                                 FACTORY_CAMERA_PREVIEW_HEIGHT,
                                 0, 0,
                                 factory_camera_quantized_scale(crop_size, FACTORY_CAMERA_PREVIEW_WIDTH),
                                 factory_camera_quantized_scale(crop_size, FACTORY_CAMERA_PREVIEW_HEIGHT),
                                 input_color_mode);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PPA preview scale failed (%s)", esp_err_to_name(ret));
        return;
    }

    if (bsp_display_lock(-1)) {
        factory_camera_copy_preview_to_canvas((uint8_t *)canvas_buf, canvas_stride,
                                              (const uint8_t *)camera->preview_buf, false);
        lv_obj_invalidate(canvas);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Skip camera preview because LVGL lock is unavailable");
    }
}

static void factory_camera_render_preview_direct(const bsp_camera_format_t *format,
                                                 const bsp_camera_frame_t *frame,
                                                 lv_obj_t *canvas,
                                                 void *canvas_buf,
                                                 uint32_t canvas_stride)
{
    if (!factory_camera_frame_matches_preview(format)) {
        ESP_LOGW(TAG, "Direct camera preview requires %"PRIu32"x%"PRIu32", got %"PRIu32"x%"PRIu32,
                 (uint32_t)FACTORY_CAMERA_PREVIEW_WIDTH,
                 (uint32_t)FACTORY_CAMERA_PREVIEW_HEIGHT,
                 format ? format->width : 0,
                 format ? format->height : 0);
        return;
    }

    const uint32_t src_stride = factory_camera_get_stride(format);
    if (bsp_display_lock(-1)) {
        factory_camera_copy_direct_preview_to_canvas((uint8_t *)canvas_buf, canvas_stride,
                                                     (const uint8_t *)frame->data, src_stride,
                                                     factory_camera_needs_byte_swap(format->pixelformat));
        lv_obj_invalidate(canvas);
        bsp_display_unlock();
    } else {
        ESP_LOGW(TAG, "Skip camera preview because LVGL lock is unavailable");
    }
}

static void factory_camera_task(void *arg)
{
    factory_camera_t *camera = (factory_camera_t *)arg;
    bsp_camera_t *bsp_camera = NULL;
    bsp_camera_format_t format = {0};
    factory_camera_pipeline_t pipeline = FACTORY_CAMERA_PIPELINE_UNKNOWN;
    bool stream_started = false;
    esp_err_t ret = ESP_OK;

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    factory_camera_set_status_locked(camera, FACTORY_CAMERA_STATE_STARTING, "Opening DVP camera...");
    xSemaphoreGive(camera->lock);

    const bsp_camera_config_t config = {
        .width = 0,
        .height = 0,
        .pixel_format = BSP_CAMERA_PIXEL_FORMAT_RGB565_BE,
        .xclk_freq_hz = BSP_CAMERA_DEFAULT_XCLK_FREQ_HZ,
    };

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    bsp_camera = (bsp_camera_t *)camera->bsp_camera;
    xSemaphoreGive(camera->lock);
    if (!bsp_camera) {
        ret = bsp_camera_open(&config, &bsp_camera);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Open BSP camera failed (%s)", esp_err_to_name(ret));
            goto cleanup;
        }
        xSemaphoreTake(camera->lock, portMAX_DELAY);
        camera->bsp_camera = bsp_camera;
        xSemaphoreGive(camera->lock);
    }

    ret = bsp_camera_get_format(bsp_camera, &format);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get BSP camera format failed (%s)", esp_err_to_name(ret));
        goto cleanup;
    }
    pipeline = factory_camera_select_pipeline(&format);
    if (pipeline == FACTORY_CAMERA_PIPELINE_UNKNOWN) {
        ESP_LOGE(TAG, "Unsupported camera format: fourcc=%c%c%c%c, size=%"PRIu32"x%"PRIu32,
                 (char)(format.pixelformat & 0xff),
                 (char)((format.pixelformat >> 8) & 0xff),
                 (char)((format.pixelformat >> 16) & 0xff),
                 (char)((format.pixelformat >> 24) & 0xff),
                 format.width, format.height);
        ret = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    ESP_LOGI(TAG, "BSP camera format: %"PRIu32"x%"PRIu32", fourcc=%c%c%c%c, stride=%"PRIu32", size=%"PRIu32,
             format.width,
             format.height,
             (char)(format.pixelformat & 0xff),
             (char)((format.pixelformat >> 8) & 0xff),
             (char)((format.pixelformat >> 16) & 0xff),
             (char)((format.pixelformat >> 24) & 0xff),
             format.bytesperline,
             format.sizeimage);
    ESP_LOGI(TAG, "Camera preview pipeline: %s", factory_camera_pipeline_name(pipeline));

    if (pipeline != FACTORY_CAMERA_PIPELINE_RGB565_DIRECT) {
        ret = factory_camera_register_ppa(camera);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = factory_camera_alloc_buffers(camera, pipeline, &format);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }

    ret = bsp_camera_start(bsp_camera);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start BSP camera failed (%s)", esp_err_to_name(ret));
        goto cleanup;
    }
    stream_started = true;

    const bool sensor_rotate_180 = pipeline != FACTORY_CAMERA_PIPELINE_YUV422_UYVY;
    esp_err_t orientation_ret = bsp_camera_set_orientation(bsp_camera, sensor_rotate_180, sensor_rotate_180);
    if (orientation_ret != ESP_OK) {
        ESP_LOGW(TAG, "Sensor orientation control failed (%s); using sensor-native orientation",
                 esp_err_to_name(orientation_ret));
    }

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    camera->frame_width = format.width;
    camera->frame_height = format.height;
    camera->pixelformat = format.pixelformat;
    factory_camera_set_status_locked(camera, FACTORY_CAMERA_STATE_PREVIEW, "DVP camera running");
    xSemaphoreGive(camera->lock);

    while (1) {
        xSemaphoreTake(camera->lock, portMAX_DELAY);
        bool stop_requested = camera->stop_requested;
        lv_obj_t *canvas = camera->canvas;
        void *canvas_buf = camera->canvas_buf;
        uint32_t canvas_stride = camera->canvas_stride;
        bool render_enabled = camera->render_enabled;
        xSemaphoreGive(camera->lock);
        if (stop_requested) {
            break;
        }

        static bool s_dqbuf_logged = false;
        if (!s_dqbuf_logged) {
            ESP_LOGI(TAG, "Waiting for first DVP frame...");
            s_dqbuf_logged = true;
        }

        bsp_camera_frame_t frame = {0};
        ret = bsp_camera_get_frame(bsp_camera, &frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Get BSP camera frame failed (%s)", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        static bool s_first_frame_logged = false;
        if (!s_first_frame_logged) {
            ESP_LOGI(TAG, "First frame: data=%p size=%zu", frame.data, frame.size);
            s_first_frame_logged = true;
        }

        const bool render_preview = render_enabled && canvas && canvas_buf;
        const size_t expected_frame_size = (size_t)factory_camera_get_stride(&format) * format.height;
        if (render_preview && frame.data &&
                (frame.size == 0 || frame.size >= expected_frame_size)) {
            if (pipeline == FACTORY_CAMERA_PIPELINE_RGB565_DIRECT) {
                factory_camera_render_preview_direct(&format, &frame, canvas, canvas_buf, canvas_stride);
            } else {
                factory_camera_render_preview(camera, pipeline, &format, &frame,
                                              canvas, canvas_buf, canvas_stride);
            }
        }

        ret = bsp_camera_return_frame(bsp_camera, &frame);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Return BSP camera frame failed (%s)", esp_err_to_name(ret));
        }

        xSemaphoreTake(camera->lock, portMAX_DELAY);
        camera->frame_count++;
        xSemaphoreGive(camera->lock);
    }

    ret = ESP_OK;

cleanup:
    if (stream_started) {
        esp_err_t stop_ret = bsp_camera_stop(bsp_camera);
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "Stop BSP camera failed (%s)", esp_err_to_name(stop_ret));
        }
    }
    xSemaphoreTake(camera->lock, portMAX_DELAY);
    camera->stop_requested = false;
    if (ret == ESP_OK) {
        factory_camera_set_status_locked(camera, FACTORY_CAMERA_STATE_IDLE, "Ready for DVP camera");
    } else {
        factory_camera_set_status_locked(camera, FACTORY_CAMERA_STATE_ERROR, "DVP camera failed");
    }
    camera->task = NULL;
    xSemaphoreGive(camera->lock);

    if (camera->ppa_srm) {
        esp_err_t ppa_ret = ppa_unregister_client(camera->ppa_srm);
        if (ppa_ret != ESP_OK) {
            ESP_LOGW(TAG, "Unregister PPA SRM client failed (%s)", esp_err_to_name(ppa_ret));
        }
        camera->ppa_srm = NULL;
    }
    factory_camera_free_buffers(camera);

    vTaskDelete(NULL);
}

esp_err_t factory_camera_init(factory_camera_t *camera, lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "invalid camera init arguments");

    memset(camera, 0, sizeof(*camera));
    camera->lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(camera->lock, ESP_ERR_NO_MEM, TAG, "camera mutex alloc failed");
    camera->disp = disp;
    camera->render_enabled = true;
    factory_camera_set_status(camera, FACTORY_CAMERA_STATE_IDLE, "Ready for DVP camera");

    ESP_LOGI(TAG, "Camera preview service initialized");
    return ESP_OK;
}

esp_err_t factory_camera_attach_canvas(factory_camera_t *camera, lv_obj_t *canvas, void *canvas_buf,
                                       uint32_t canvas_width, uint32_t canvas_height, uint32_t canvas_stride)
{
    ESP_RETURN_ON_FALSE(camera && canvas && canvas_buf, ESP_ERR_INVALID_ARG, TAG, "invalid camera canvas arguments");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    camera->canvas = canvas;
    camera->canvas_buf = canvas_buf;
    camera->canvas_width = canvas_width;
    camera->canvas_height = canvas_height;
    camera->canvas_stride = canvas_stride;
    xSemaphoreGive(camera->lock);

    ESP_LOGI(TAG, "Camera canvas attached: %"PRIu32"x%"PRIu32", stride=%"PRIu32,
             canvas_width, canvas_height, canvas_stride);
    return ESP_OK;
}

esp_err_t factory_camera_start_preview(factory_camera_t *camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (camera->task || camera->state == FACTORY_CAMERA_STATE_STARTING || camera->state == FACTORY_CAMERA_STATE_PREVIEW) {
        xSemaphoreGive(camera->lock);
        return ESP_ERR_INVALID_STATE;
    }

    camera->stop_requested = false;
    camera->frame_count = 0;
    camera->frame_width = 0;
    camera->frame_height = 0;
    camera->pixelformat = 0;
    factory_camera_set_status_locked(camera, FACTORY_CAMERA_STATE_STARTING, "Starting DVP camera...");
    xSemaphoreGive(camera->lock);

    BaseType_t ok = xTaskCreate(factory_camera_task, "factory_camera", FACTORY_CAMERA_TASK_STACK,
                                camera, FACTORY_CAMERA_TASK_PRIORITY, &camera->task);
    if (ok != pdPASS) {
        xSemaphoreTake(camera->lock, portMAX_DELAY);
        factory_camera_set_status_locked(camera, FACTORY_CAMERA_STATE_ERROR, "Preview task create failed");
        camera->task = NULL;
        xSemaphoreGive(camera->lock);
        ESP_LOGE(TAG, "Camera task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Camera preview requested");
    return ESP_OK;
}

esp_err_t factory_camera_stop_preview(factory_camera_t *camera)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    if (!camera->task) {
        xSemaphoreGive(camera->lock);
        return ESP_OK;
    }

    camera->stop_requested = true;
    xSemaphoreGive(camera->lock);

    ESP_LOGI(TAG, "Camera stop requested");
    return ESP_OK;
}

esp_err_t factory_camera_set_display(factory_camera_t *camera, lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    camera->disp = disp;
    xSemaphoreGive(camera->lock);
    return ESP_OK;
}

esp_err_t factory_camera_set_render_enabled(factory_camera_t *camera, bool enable)
{
    ESP_RETURN_ON_FALSE(camera, ESP_ERR_INVALID_ARG, TAG, "camera handle is null");

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    camera->render_enabled = enable;
    xSemaphoreGive(camera->lock);
    ESP_LOGI(TAG, "Camera rendering %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

void factory_camera_get_snapshot(factory_camera_t *camera, factory_camera_snapshot_t *snapshot)
{
    if (!camera || !snapshot) {
        return;
    }

    xSemaphoreTake(camera->lock, portMAX_DELAY);
    snapshot->state = camera->state;
    snapshot->frame_width = camera->frame_width;
    snapshot->frame_height = camera->frame_height;
    snapshot->pixelformat = camera->pixelformat;
    snapshot->frame_count = camera->frame_count;
    strlcpy(snapshot->status, camera->status, sizeof(snapshot->status));
    xSemaphoreGive(camera->lock);
}
