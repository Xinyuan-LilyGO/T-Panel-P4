#include "libs/tjpgd/tjpgd.h"
#include "page_common.h"
#include <app/services/camera_control.h>
#include <dirent.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define CAMERA_WIDTH 720
#define CAMERA_HEIGHT 720
#define CAMERA_MARGIN 40
#define CAMERA_BOTTOM_MARGIN 28
#define CAMERA_CYBER_BG CYBER_BG
#define CAMERA_CYBER_PANEL CYBER_PANEL
#define CAMERA_CYBER_GREEN CYBER_GREEN
#define CAMERA_CYBER_CYAN CYBER_CYAN
#define CAMERA_CYBER_YELLOW CYBER_YELLOW
#define CAMERA_CYBER_MAGENTA CYBER_MAGENTA
#define CAMERA_CYBER_TEXT CYBER_TEXT
#define CAMERA_CYBER_MUTED CYBER_MUTED
#define CAMERA_CYBER_GRID CYBER_GRID

static lv_obj_t *s_camera_status;
static lv_obj_t *s_camera_capture;
static lv_obj_t *s_camera_resolution;
static lv_obj_t *s_camera_fps;
static lv_obj_t *s_camera_image;
static lv_obj_t *s_camera_preview_container;
static lv_obj_t *s_camera_thumbnail;
static lv_obj_t *s_camera_photo_preview;
static lv_obj_t *s_camera_delete;
static lv_obj_t *s_camera_previous;
static lv_obj_t *s_camera_next;
static lv_timer_t *s_camera_frame_timer;
static bool s_camera_capture_pending;
static uint16_t *s_camera_pixels;
static uint16_t *s_camera_thumb_pixels;
static uint16_t *s_camera_preview_pixels;
static lv_image_dsc_t s_camera_frame_dsc;
static lv_image_dsc_t s_camera_thumb_dsc;
static lv_image_dsc_t s_camera_preview_dsc;
static char s_camera_last_photo_path[192];
static char s_camera_photo_paths[128][192];
static size_t s_camera_photo_count;
static size_t s_camera_photo_index;
static lv_obj_t *s_camera_preview_image;
static uint16_t *s_camera_decode_pixels;
static volatile bool s_camera_decode_busy;
static volatile bool s_camera_decode_done;
static volatile bool s_camera_decode_ok;
static size_t s_camera_decode_index;
static char s_camera_decode_path[192];
static volatile bool s_camera_decode_queued;
static volatile size_t s_camera_decode_queued_index;
static volatile bool s_camera_delete_status_pending;

#define CAMERA_PREVIEW_WIDTH (CAMERA_WIDTH - CAMERA_MARGIN * 2)
#define CAMERA_PREVIEW_HEIGHT 360
#define CAMERA_MAX_PHOTOS 128

static const char *TAG = "factory_camera_ui";

static int camera_photo_compare(const void *left, const void *right)
{
    return strcmp((const char *)left, (const char *)right);
}

static void camera_scan_photos(void)
{
    s_camera_photo_count = 0U;
    DIR *directory = opendir("/sdcard/camera");
    if (directory == NULL)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL && s_camera_photo_count < CAMERA_MAX_PHOTOS)
    {
        const char *name = entry->d_name;
        const size_t length = strlen(name);
        if (length < 10U || strncmp(name, "PHOTO_", 6U) != 0 ||
            strcasecmp(name + length - 4U, ".JPG") != 0)
            continue;
        snprintf(s_camera_photo_paths[s_camera_photo_count],
                 sizeof(s_camera_photo_paths[0]), "/sdcard/camera/%s", name);
        ++s_camera_photo_count;
    }
    closedir(directory);
    qsort(s_camera_photo_paths, s_camera_photo_count,
          sizeof(s_camera_photo_paths[0]), camera_photo_compare);
}

typedef struct {
    FILE *file;
    uint32_t source_width;
    uint32_t source_height;
    uint16_t *destination;
} camera_jpeg_context_t;

static size_t camera_jpeg_input(JDEC *decoder, uint8_t *buffer, size_t size)
{
    camera_jpeg_context_t *context = static_cast<camera_jpeg_context_t *>(decoder->device);
    if (buffer == NULL)
        return fseek(context->file, (long)size, SEEK_CUR) == 0U ? size : 0U;
    return fread(buffer, 1, size, context->file);
}

static int camera_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    camera_jpeg_context_t *context = static_cast<camera_jpeg_context_t *>(decoder->device);
    const uint8_t *source = static_cast<const uint8_t *>(bitmap);
    const uint32_t block_width = (uint32_t)rect->right - rect->left + 1U;
    const uint32_t destination_left = ((uint32_t)rect->left * CAMERA_PREVIEW_WIDTH +
                                       context->source_width - 1U) /
                                      context->source_width;
    const uint32_t destination_right = (((uint32_t)rect->right + 1U) *
                                         CAMERA_PREVIEW_WIDTH + context->source_width - 1U) /
                                        context->source_width;
    const uint32_t destination_top = ((uint32_t)rect->top * CAMERA_PREVIEW_HEIGHT +
                                      context->source_height - 1U) /
                                     context->source_height;
    const uint32_t destination_bottom = (((uint32_t)rect->bottom + 1U) *
                                         CAMERA_PREVIEW_HEIGHT + context->source_height - 1U) /
                                        context->source_height;
    for (uint32_t y = destination_top; y < destination_bottom &&
                                       y < CAMERA_PREVIEW_HEIGHT; ++y)
    {
        const uint32_t source_y = y * context->source_height / CAMERA_PREVIEW_HEIGHT;
        for (uint32_t x = destination_left; x < destination_right &&
                                           x < CAMERA_PREVIEW_WIDTH; ++x)
        {
            const uint32_t source_x = x * context->source_width / CAMERA_PREVIEW_WIDTH;
            const uint8_t *pixel = source + (((source_y - rect->top) * block_width +
                                             source_x - rect->left) * 3U);
            /* TJpgDec emits pixels as BGR888 (see JD_FORMAT conversion). */
            context->destination[y * CAMERA_PREVIEW_WIDTH + x] =
                (uint16_t)(((pixel[2] >> 3U) << 11U) |
                           ((pixel[1] >> 2U) << 5U) |
                           (pixel[0] >> 3U));
        }
    }
    return 1;
}

static bool camera_load_jpeg(const char *path, uint16_t *destination)
{
    if (path == NULL || destination == NULL)
        return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        ESP_LOGW(TAG, "open jpeg failed path=%s errno=%d", path, errno);
        return false;
    }
    uint8_t *work = static_cast<uint8_t *>(malloc(4096U));
    if (work == NULL)
    {
        fclose(file);
        return false;
    }
    memset(destination, 0, (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT *
                           sizeof(uint16_t));
    camera_jpeg_context_t context = {.file = file, .destination = destination};
    JDEC decoder;
    JRESULT prepared = jd_prepare(&decoder, camera_jpeg_input, work, 4096U, &context);
    if (prepared != JDR_OK)
    {
        ESP_LOGW(TAG, "jpeg prepare failed path=%s result=%d", path, prepared);
        free(work);
        fclose(file);
        return false;
    }
    context.source_width = decoder.width;
    context.source_height = decoder.height;
    JRESULT decoded = jd_decomp(&decoder, camera_jpeg_output, 0U);
    free(work);
    fclose(file);
    if (decoded != JDR_OK)
        ESP_LOGW(TAG, "jpeg decode failed path=%s result=%d size=%ux%u",
                 path, decoded, decoder.width, decoder.height);
    return decoded == JDR_OK;
}

static void camera_apply_photo(size_t index)
{
    if (index >= s_camera_photo_count)
        return;
    s_camera_photo_index = index;
    snprintf(s_camera_last_photo_path, sizeof(s_camera_last_photo_path),
             "%s", s_camera_photo_paths[index]);
    if (s_camera_preview_image != NULL)
        lv_obj_invalidate(s_camera_preview_image);
    if (s_camera_thumbnail != NULL && s_camera_thumb_pixels != NULL)
    {
        for (uint32_t y = 0U; y < 90U; ++y)
            for (uint32_t x = 0U; x < 160U; ++x)
                s_camera_thumb_pixels[y * 160U + x] =
                    s_camera_preview_pixels[(y * CAMERA_PREVIEW_HEIGHT / 90U) *
                                            CAMERA_PREVIEW_WIDTH +
                                            (x * CAMERA_PREVIEW_WIDTH / 160U)];
        lv_image_set_src(s_camera_thumbnail, &s_camera_thumb_dsc);
    }
}

static void camera_update_navigation_state(void)
{
    const bool has_previous = s_camera_photo_count > 1U && s_camera_photo_index > 0U;
    const bool has_next = s_camera_photo_count > 1U &&
                          s_camera_photo_index + 1U < s_camera_photo_count;
    if (s_camera_previous != NULL)
    {
        if (has_previous)
            lv_obj_clear_state(s_camera_previous, LV_STATE_DISABLED);
        else
            lv_obj_add_state(s_camera_previous, LV_STATE_DISABLED);
    }
    if (s_camera_next != NULL)
    {
        if (has_next)
            lv_obj_clear_state(s_camera_next, LV_STATE_DISABLED);
        else
            lv_obj_add_state(s_camera_next, LV_STATE_DISABLED);
    }
}

static void camera_decode_task(void *argument)
{
    const size_t index = (size_t)(uintptr_t)argument;
    char path[sizeof(s_camera_decode_path)];
    snprintf(path, sizeof(path), "%s", s_camera_decode_path);
    bool ok = index < s_camera_photo_count &&
              camera_load_jpeg(path, s_camera_decode_pixels);
    if (!ok)
    {
        vTaskDelay(pdMS_TO_TICKS(20U));
        ok = camera_load_jpeg(path, s_camera_decode_pixels);
    }
    s_camera_decode_index = index;
    s_camera_decode_ok = ok;
    ESP_LOGI(TAG, "jpeg decode finished path=%s ok=%d index=%u",
             path, ok ? 1 : 0, (unsigned)index);
    s_camera_decode_done = true;
    vTaskDelete(NULL);
}

static void camera_request_photo(size_t index)
{
    if (index >= s_camera_photo_count || s_camera_decode_pixels == NULL)
        return;
    if (s_camera_decode_busy)
    {
        s_camera_decode_queued_index = index;
        s_camera_decode_queued = true;
        if (s_camera_status != NULL)
            lv_label_set_text(s_camera_status, "PHOTO // QUEUED");
        return;
    }
    s_camera_photo_index = index;
    snprintf(s_camera_last_photo_path, sizeof(s_camera_last_photo_path),
             "%s", s_camera_photo_paths[index]);
    snprintf(s_camera_decode_path, sizeof(s_camera_decode_path),
             "%s", s_camera_photo_paths[index]);
    if (s_camera_delete != NULL)
        lv_obj_add_state(s_camera_delete, LV_STATE_DISABLED);
    s_camera_decode_busy = true;
    s_camera_decode_done = false;
    if (s_camera_status != NULL)
        lv_label_set_text(s_camera_status,
                          s_camera_photo_preview != NULL &&
                                  !lv_obj_has_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN)
                              ? "PREVIEW // DECODING"
                              : "PHOTO // DECODING");
    if (xTaskCreate(camera_decode_task, "camera_jpeg", 8192,
                    (void *)(uintptr_t)index, 5, NULL) != pdPASS)
        s_camera_decode_busy = false;
}

static void camera_previous_cb(lv_event_t *event)
{
    (void)event;
    if (s_camera_photo_index > 0U)
        camera_request_photo(s_camera_photo_index - 1U);
    else if (s_camera_status != NULL)
        lv_label_set_text(s_camera_status, "PHOTO // FIRST");
}

static void camera_next_cb(lv_event_t *event)
{
    (void)event;
    if (s_camera_photo_index + 1U < s_camera_photo_count)
        camera_request_photo(s_camera_photo_index + 1U);
    else if (s_camera_status != NULL)
        lv_label_set_text(s_camera_status, "PHOTO // LAST");
}

static void camera_gesture_passthrough(lv_obj_t *object)
{
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static lv_obj_t *camera_line(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t width, lv_coord_t height, uint32_t color,
                             lv_opa_t opacity)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, width, height);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, opacity, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_shadow_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(line, 5, 0);
    lv_obj_set_style_shadow_opa(line, LV_OPA_30, 0);
    camera_gesture_passthrough(line);
    return line;
}

static void camera_corner_marks(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t width, lv_coord_t height)
{
    const lv_coord_t left = x - 6;
    const lv_coord_t top = y - 6;
    const lv_coord_t right = x + width + 3;
    const lv_coord_t bottom = y + height + 3;
    const uint32_t color = CAMERA_CYBER_YELLOW;
    camera_line(parent, left, top, 30, 3, color, LV_OPA_COVER);
    camera_line(parent, left, top, 3, 30, color, LV_OPA_COVER);
    camera_line(parent, right - 27, top, 30, 3, color, LV_OPA_COVER);
    camera_line(parent, right, top, 3, 30, color, LV_OPA_COVER);
    camera_line(parent, left, bottom, 30, 3, color, LV_OPA_COVER);
    camera_line(parent, left, bottom - 27, 3, 30, color, LV_OPA_COVER);
    camera_line(parent, right - 27, bottom, 30, 3, color, LV_OPA_COVER);
    camera_line(parent, right, bottom - 27, 3, 30, color, LV_OPA_COVER);
}

static void camera_capture_cb(lv_event_t *event)
{
    lv_obj_t *status = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    char path[192];
    if (camera_control_capture_photo(path, sizeof(path)))
    {
        s_camera_capture_pending = true;
        if (status != NULL)
            lv_label_set_text(status, "CAPTURE // SAVING");
        if (s_camera_capture != NULL)
            lv_obj_add_state(s_camera_capture, LV_STATE_DISABLED);
    }
    else if (status != NULL)
        lv_label_set_text(status, "CAPTURE // BUSY");
}

static void camera_photo_preview_cb(lv_event_t *event)
{
    (void)event;
    if (s_camera_photo_preview != NULL)
    {
        if (lv_obj_has_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_remove_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN);
            if (s_camera_status != NULL)
                lv_label_set_text(s_camera_status,
                                  s_camera_decode_busy ? "PREVIEW // DECODING" :
                                                         "PREVIEW // OPEN");
        }
        else
        {
            lv_obj_add_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN);
            if (s_camera_status != NULL)
                lv_label_set_text(s_camera_status, "PREVIEW // CLOSED");
        }
    }
}

static void camera_delete_cb(lv_event_t *event)
{
    (void)event;
    if (s_camera_last_photo_path[0] == '\0')
        return;
    if (s_camera_decode_busy)
    {
        if (s_camera_status != NULL)
            lv_label_set_text(s_camera_status, "PHOTO // BUSY");
        return;
    }
    if (s_camera_status != NULL)
        lv_label_set_text(s_camera_status, "PHOTO // DELETING");
    if (camera_control_delete_photo(s_camera_last_photo_path))
    {
        ESP_LOGI(TAG, "photo deleted path=%s", s_camera_last_photo_path);
        s_camera_delete_status_pending = true;
        s_camera_last_photo_path[0] = '\0';
        camera_scan_photos();
        if (s_camera_photo_count > 0U)
        {
            if (s_camera_photo_index >= s_camera_photo_count)
                s_camera_photo_index = s_camera_photo_count - 1U;
            camera_request_photo(s_camera_photo_index);
        }
        if (s_camera_photo_count == 0U && s_camera_thumbnail != NULL)
            lv_image_set_src(s_camera_thumbnail, NULL);
        camera_update_navigation_state();
        if (s_camera_photo_preview != NULL)
            lv_obj_add_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN);
        if (s_camera_delete != NULL)
            lv_obj_add_state(s_camera_delete, LV_STATE_DISABLED);
        if (s_camera_status != NULL)
            lv_label_set_text(s_camera_status, "PHOTO // DELETED");
    }
    else if (s_camera_status != NULL)
    {
        ESP_LOGW(TAG, "photo delete failed path=%s errno=%d",
                 s_camera_last_photo_path, errno);
        lv_label_set_text(s_camera_status, "PHOTO // DELETE FAILED");
    }
}

static void camera_frame_update_timer(lv_timer_t *timer)
{
    (void)timer;
    if (s_camera_image == NULL || s_camera_pixels == NULL)
        return;
    if (s_camera_decode_done)
    {
        s_camera_decode_done = false;
        if (s_camera_decode_ok)
        {
            uint16_t *decoded = s_camera_preview_pixels;
            s_camera_preview_pixels = s_camera_decode_pixels;
            s_camera_decode_pixels = decoded;
            s_camera_preview_dsc.data = (const uint8_t *)s_camera_preview_pixels;
            lv_image_set_src(s_camera_preview_image, &s_camera_preview_dsc);
            camera_apply_photo(s_camera_decode_index);
            camera_update_navigation_state();
        }
        if (s_camera_status != NULL && s_camera_delete_status_pending)
        {
            s_camera_delete_status_pending = false;
            lv_label_set_text(s_camera_status, "PHOTO // DELETED");
        }
        else if (s_camera_status != NULL)
            lv_label_set_text(s_camera_status,
                              s_camera_decode_ok ?
                                  (s_camera_photo_preview != NULL &&
                                           !lv_obj_has_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN)
                                       ? "PREVIEW // READY" : "PHOTO // READY") :
                                  "PREVIEW // DECODE FAILED");
        /* The file can still be deleted when decoding fails. */
        if (s_camera_delete != NULL && s_camera_last_photo_path[0] != '\0')
            lv_obj_clear_state(s_camera_delete, LV_STATE_DISABLED);
        s_camera_decode_busy = false;
        if (s_camera_decode_queued)
        {
            const size_t queued_index = s_camera_decode_queued_index;
            s_camera_decode_queued = false;
            camera_request_photo(queued_index);
        }
    }
    if (s_camera_capture_pending)
    {
        char path[192];
        const int capture_status = camera_control_capture_status(path, sizeof(path));
        if (capture_status != 0)
        {
            s_camera_capture_pending = false;
            if (s_camera_status != NULL)
                lv_label_set_text_fmt(s_camera_status,
                                      capture_status > 0 ? "CAPTURED // %s" : "CAPTURE // FAILED",
                                      capture_status > 0 ? path : "");
            if (capture_status > 0 && s_camera_thumb_pixels != NULL &&
                s_camera_pixels != NULL)
            {
                if (s_camera_preview_pixels != NULL)
                    memcpy(s_camera_preview_pixels, s_camera_pixels,
                           (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT *
                               sizeof(uint16_t));
                for (uint32_t y = 0; y < 90U; ++y)
                {
                    const uint32_t source_y = y * CAMERA_PREVIEW_HEIGHT / 90U;
                    for (uint32_t x = 0; x < 160U; ++x)
                    {
                        const uint32_t source_x = x * CAMERA_PREVIEW_WIDTH / 160U;
                        s_camera_thumb_pixels[y * 160U + x] =
                            s_camera_pixels[source_y * CAMERA_PREVIEW_WIDTH + source_x];
                    }
                }
                snprintf(s_camera_last_photo_path, sizeof(s_camera_last_photo_path),
                         "%s", path);
                camera_scan_photos();
                s_camera_photo_index = s_camera_photo_count > 0U ?
                                       s_camera_photo_count - 1U : 0U;
                if (s_camera_thumbnail != NULL)
                    lv_image_set_src(s_camera_thumbnail, &s_camera_thumb_dsc);
                if (s_camera_delete != NULL)
                    lv_obj_clear_state(s_camera_delete, LV_STATE_DISABLED);
            }
            if (s_camera_capture != NULL && camera_control_last_error_name() != NULL)
                lv_obj_clear_state(s_camera_capture, LV_STATE_DISABLED);
        }
    }
    if (s_camera_photo_preview != NULL &&
        !lv_obj_has_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN))
        return;
    uint16_t width = 0U;
    uint16_t height = 0U;
    uint8_t fps = 0U;
    if (camera_control_get_mode(&width, &height, &fps))
    {
        if (s_camera_resolution != NULL)
            lv_label_set_text_fmt(s_camera_resolution, "FRAME // %u x %u", width, height);
        if (s_camera_fps != NULL)
            lv_label_set_text_fmt(s_camera_fps, "%u FPS", fps);
    }
    if (camera_control_read_frame_rgb565(s_camera_pixels,
                                         (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT,
                                         CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT))
        lv_obj_invalidate(s_camera_image);
}

static void camera_page_create(Page *page)
{
    s_camera_decode_busy = false;
    s_camera_decode_done = false;
    s_camera_decode_queued = false;
    s_camera_delete_status_pending = false;
    lv_obj_set_style_bg_color(page->root, lv_color_hex(CAMERA_CYBER_BG), 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(page->root, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(page->root, LV_OBJ_FLAG_SCROLL_CHAIN_VER);

    const lv_coord_t preview_x = CAMERA_MARGIN;
    const lv_coord_t preview_y = 54;
    const lv_coord_t preview_w = CAMERA_WIDTH - CAMERA_MARGIN * 2;
    const lv_coord_t preview_h = CAMERA_PREVIEW_HEIGHT;
    lv_obj_t *preview = lv_obj_create(page->root);
    lv_obj_set_size(preview, preview_w, preview_h);
    lv_obj_set_pos(preview, preview_x, preview_y);
    lv_obj_set_style_bg_color(preview, lv_color_hex(CAMERA_CYBER_PANEL), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_80, 0);
    lv_obj_set_style_border_color(preview, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, 0, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_set_scrollbar_mode(preview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    s_camera_preview_container = preview;
    lv_obj_set_style_shadow_color(preview, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_shadow_width(preview, 14, 0);
    lv_obj_set_style_shadow_opa(preview, LV_OPA_20, 0);
    camera_gesture_passthrough(preview);
    camera_corner_marks(page->root, preview_x, preview_y, preview_w, preview_h);

    s_camera_pixels = static_cast<uint16_t *>(heap_caps_malloc(
        (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_camera_image = lv_image_create(preview);
    lv_obj_set_size(s_camera_image, CAMERA_PREVIEW_WIDTH, CAMERA_PREVIEW_HEIGHT);
    lv_obj_set_pos(s_camera_image, 0, 0);
    lv_obj_set_style_opa(s_camera_image, LV_OPA_COVER, 0);
    if (s_camera_pixels != NULL)
    {
        memset(s_camera_pixels, 0, (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT * sizeof(uint16_t));
        memset(&s_camera_frame_dsc, 0, sizeof(s_camera_frame_dsc));
        s_camera_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_camera_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_camera_frame_dsc.header.w = CAMERA_PREVIEW_WIDTH;
        s_camera_frame_dsc.header.h = CAMERA_PREVIEW_HEIGHT;
        s_camera_frame_dsc.header.stride = CAMERA_PREVIEW_WIDTH * sizeof(uint16_t);
        s_camera_frame_dsc.data_size = CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT * sizeof(uint16_t);
        s_camera_frame_dsc.data = (const uint8_t *)s_camera_pixels;
        lv_image_set_src(s_camera_image, &s_camera_frame_dsc);
    }
    camera_gesture_passthrough(s_camera_image);

    s_camera_thumb_pixels = static_cast<uint16_t *>(heap_caps_calloc(
        (size_t)160U * 90U, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_camera_preview_pixels = static_cast<uint16_t *>(heap_caps_malloc(
        (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_camera_decode_pixels = static_cast<uint16_t *>(heap_caps_malloc(
        (size_t)CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    memset(&s_camera_thumb_dsc, 0, sizeof(s_camera_thumb_dsc));
    s_camera_thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_camera_thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_camera_thumb_dsc.header.w = 160;
    s_camera_thumb_dsc.header.h = 90;
    s_camera_thumb_dsc.header.stride = 160U * sizeof(uint16_t);
    s_camera_thumb_dsc.data_size = 160U * 90U * sizeof(uint16_t);
    s_camera_thumb_dsc.data = (const uint8_t *)s_camera_thumb_pixels;
    memset(&s_camera_preview_dsc, 0, sizeof(s_camera_preview_dsc));
    s_camera_preview_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_camera_preview_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_camera_preview_dsc.header.w = CAMERA_PREVIEW_WIDTH;
    s_camera_preview_dsc.header.h = CAMERA_PREVIEW_HEIGHT;
    s_camera_preview_dsc.header.stride = CAMERA_PREVIEW_WIDTH * sizeof(uint16_t);
    s_camera_preview_dsc.data_size = CAMERA_PREVIEW_WIDTH * CAMERA_PREVIEW_HEIGHT *
                                     sizeof(uint16_t);
    s_camera_preview_dsc.data = (const uint8_t *)s_camera_preview_pixels;

    s_camera_status = page_ui_label(page->root, "CAMERA // NOT ACTIVE",
                                    &lv_font_ShareTechMono1_16, CAMERA_CYBER_MUTED);
    lv_obj_set_width(s_camera_status, 440);
    lv_label_set_long_mode(s_camera_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_camera_status, CAMERA_MARGIN, 488);

    s_camera_resolution = page_ui_label(page->root, "FRAME // -- x --",
                                        &lv_font_ShareTechMono1_16,
                                        CAMERA_CYBER_MUTED);
    lv_obj_set_pos(s_camera_resolution, CAMERA_MARGIN, 442);
    s_camera_fps = page_ui_label(page->root, "-- FPS",
                                 &lv_font_ShareTechMono1_16, CAMERA_CYBER_GREEN);
    lv_obj_set_width(s_camera_fps, 130);
    lv_obj_set_style_text_align(s_camera_fps, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_camera_fps, CAMERA_WIDTH - CAMERA_MARGIN - 130, 442);

    s_camera_capture = page_ui_button(page->root, LV_SYMBOL_IMAGE, "CAPTURE",
                                      176, 48, camera_capture_cb, s_camera_status);
    lv_obj_set_pos(s_camera_capture, CAMERA_WIDTH - CAMERA_MARGIN - 176, 488);
    lv_obj_set_style_bg_color(s_camera_capture, lv_color_hex(CAMERA_CYBER_PANEL), 0);
    lv_obj_set_style_bg_color(s_camera_capture, lv_color_hex(CAMERA_CYBER_CYAN), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_capture, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_camera_capture, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_camera_capture, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_border_width(s_camera_capture, 1, 0);
    lv_obj_set_style_border_opa(s_camera_capture, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_camera_capture, 0, 0);
    lv_obj_set_style_shadow_color(s_camera_capture, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_shadow_width(s_camera_capture, 10, 0);
    lv_obj_set_style_shadow_opa(s_camera_capture, LV_OPA_30, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_camera_capture, 0),
                               &lv_font_ShareTechMono1_20, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_camera_capture, 0),
                                lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_add_state(s_camera_capture, LV_STATE_DISABLED);
    s_camera_frame_timer = lv_timer_create(camera_frame_update_timer, 100, NULL);

    lv_obj_t *storage = page_ui_label(page->root, "OUTPUT // SD:/CAMERA",
                                      &lv_font_ShareTechMono1_16, CAMERA_CYBER_CYAN);
    lv_obj_set_pos(storage, CAMERA_MARGIN, 506);
    lv_obj_t *capture_caption = page_ui_label(page->root, "CAPTURE FRAME",
                                              &lv_font_ShareTechMono1_16,
                                              CAMERA_CYBER_YELLOW);
    lv_obj_set_pos(capture_caption, CAMERA_MARGIN, 542);
    lv_obj_t *mode = page_ui_label(page->root, "PHOTO MODE  /  RGB565 BMP",
                                   &lv_font_ShareTechMono1_16, CAMERA_CYBER_MUTED);
    lv_obj_set_width(mode, 250);
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(mode, CAMERA_WIDTH - CAMERA_MARGIN - 250, 542);

    lv_obj_t *recent = page_ui_label(page->root, "LAST CAPTURE",
                                     &lv_font_ShareTechMono1_16, CAMERA_CYBER_YELLOW);
    lv_obj_set_pos(recent, CAMERA_MARGIN, 580);
    s_camera_thumbnail = lv_image_create(page->root);
    lv_obj_set_size(s_camera_thumbnail, 160, 90);
    lv_obj_set_pos(s_camera_thumbnail, CAMERA_MARGIN, 600);
    lv_obj_add_flag(s_camera_thumbnail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_camera_thumbnail, camera_photo_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(s_camera_thumbnail, lv_color_hex(CAMERA_CYBER_CYAN),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_thumbnail, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_camera_thumbnail, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_border_width(s_camera_thumbnail, 1, 0);
    lv_obj_set_style_radius(s_camera_thumbnail, 0, 0);
    lv_obj_set_style_bg_color(s_camera_thumbnail, lv_color_hex(CAMERA_CYBER_PANEL), 0);
    lv_obj_set_style_bg_opa(s_camera_thumbnail, LV_OPA_COVER, 0);

    s_camera_delete = page_ui_button(page->root, LV_SYMBOL_TRASH, "DELETE PHOTO",
                                     176, 58, camera_delete_cb, NULL);
    lv_obj_set_pos(s_camera_delete, CAMERA_WIDTH - CAMERA_MARGIN - 176,
                   CAMERA_HEIGHT - CAMERA_BOTTOM_MARGIN - 58);
    lv_obj_set_style_bg_color(s_camera_delete, lv_color_hex(CAMERA_CYBER_PANEL), 0);
    lv_obj_set_style_bg_color(s_camera_delete, lv_color_hex(CAMERA_CYBER_MAGENTA),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_delete, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_camera_delete, lv_color_hex(CAMERA_CYBER_MAGENTA), 0);
    lv_obj_set_style_border_width(s_camera_delete, 1, 0);
    lv_obj_set_style_border_opa(s_camera_delete, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(s_camera_delete, 0, 0);
    lv_obj_set_scrollbar_mode(s_camera_delete, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(s_camera_delete, 0, 0);
    lv_obj_set_style_pad_all(s_camera_delete, 0, 0);
    lv_obj_set_style_shadow_color(s_camera_delete, lv_color_hex(CAMERA_CYBER_MAGENTA), 0);
    lv_obj_set_style_shadow_width(s_camera_delete, 8, 0);
    lv_obj_set_style_shadow_opa(s_camera_delete, LV_OPA_30, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_camera_delete, 0),
                               &lv_font_ShareTechMono1_16, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_camera_delete, 0),
                                lv_color_hex(CAMERA_CYBER_MAGENTA), 0);
    lv_obj_add_state(s_camera_delete, LV_STATE_DISABLED);

    s_camera_previous = page_ui_button(page->root, LV_SYMBOL_LEFT, "LAST",
                                       120, 48, camera_previous_cb, NULL);
    s_camera_next = page_ui_button(page->root, LV_SYMBOL_RIGHT, "NEXT",
                                   120, 48, camera_next_cb, NULL);
    lv_obj_set_pos(s_camera_previous, 220,
                   CAMERA_HEIGHT - CAMERA_BOTTOM_MARGIN - 48);
    lv_obj_set_pos(s_camera_next, 362,
                   CAMERA_HEIGHT - CAMERA_BOTTOM_MARGIN - 48);
    lv_obj_set_style_bg_color(s_camera_previous, lv_color_hex(CAMERA_CYBER_PANEL), 0);
    lv_obj_set_style_bg_color(s_camera_next, lv_color_hex(CAMERA_CYBER_PANEL), 0);
    lv_obj_set_style_bg_color(s_camera_previous, lv_color_hex(CAMERA_CYBER_CYAN),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_camera_next, lv_color_hex(CAMERA_CYBER_CYAN),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_previous, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_next, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(s_camera_previous, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_border_color(s_camera_next, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_border_width(s_camera_previous, 1, 0);
    lv_obj_set_style_border_width(s_camera_next, 1, 0);
    lv_obj_set_style_border_opa(s_camera_previous, LV_OPA_40, 0);
    lv_obj_set_style_border_opa(s_camera_next, LV_OPA_40, 0);
    lv_obj_set_style_radius(s_camera_previous, 0, 0);
    lv_obj_set_style_radius(s_camera_next, 0, 0);
    lv_obj_set_style_pad_all(s_camera_previous, 0, 0);
    lv_obj_set_style_pad_all(s_camera_next, 0, 0);
    lv_obj_t *previous_label = lv_obj_get_child(s_camera_previous, 0);
    lv_obj_t *next_label = lv_obj_get_child(s_camera_next, 0);
    lv_obj_set_style_text_font(previous_label, &lv_font_ShareTechMono1_16, 0);
    lv_obj_set_style_text_font(next_label, &lv_font_ShareTechMono1_16, 0);
    lv_obj_set_style_text_color(previous_label, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_set_style_text_color(next_label, lv_color_hex(CAMERA_CYBER_CYAN), 0);
    lv_obj_center(previous_label);
    lv_obj_center(next_label);
    lv_obj_add_state(s_camera_previous, LV_STATE_DISABLED);
    lv_obj_add_state(s_camera_next, LV_STATE_DISABLED);

    s_camera_photo_preview = lv_obj_create(s_camera_preview_container);
    lv_obj_set_size(s_camera_photo_preview, preview_w, preview_h);
    lv_obj_center(s_camera_photo_preview);
    lv_obj_set_style_pad_all(s_camera_photo_preview, 0, 0);
    lv_obj_set_scrollbar_mode(s_camera_photo_preview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_camera_photo_preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_camera_photo_preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(s_camera_photo_preview, 0, 0);
    lv_obj_set_scrollbar_mode(s_camera_delete, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_camera_photo_preview, camera_photo_preview_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(s_camera_photo_preview, lv_color_hex(CAMERA_CYBER_BG), 0);
    lv_obj_set_style_bg_color(s_camera_photo_preview, lv_color_hex(CAMERA_CYBER_YELLOW),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_photo_preview, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_camera_photo_preview, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_camera_photo_preview, lv_color_hex(CAMERA_CYBER_YELLOW), 0);
    lv_obj_set_style_border_width(s_camera_photo_preview, 2, 0);
    lv_obj_set_style_radius(s_camera_photo_preview, 0, 0);
    lv_obj_t *preview_image = lv_image_create(s_camera_photo_preview);
    lv_obj_set_style_pad_all(preview_image, 0, 0);
    lv_obj_set_scrollbar_mode(preview_image, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(preview_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_image_set_src(preview_image, &s_camera_preview_dsc);
    s_camera_preview_image = preview_image;
    lv_obj_center(preview_image);
    lv_obj_add_flag(s_camera_photo_preview, LV_OBJ_FLAG_HIDDEN);
    camera_scan_photos();
    if (s_camera_photo_count > 0U)
    {
        s_camera_photo_index = s_camera_photo_count - 1U;
        camera_request_photo(s_camera_photo_index);
        camera_update_navigation_state();
    }
}

static void camera_page_enter(Page *page)
{
    (void)page;
    if (s_camera_frame_timer != NULL)
        lv_timer_resume(s_camera_frame_timer);
    if (s_camera_status != NULL)
        lv_label_set_text(s_camera_status, "CAMERA // INITIALIZING OV2710...");
    if (s_camera_capture != NULL)
        lv_obj_add_state(s_camera_capture, LV_STATE_DISABLED);

    if (camera_control_enter())
    {
        if (s_camera_status != NULL)
            lv_label_set_text(s_camera_status, "CAMERA // ACTIVE  /  SENSOR LINKED");
        if (s_camera_capture != NULL)
            lv_obj_remove_state(s_camera_capture, LV_STATE_DISABLED);
    }
    else if (s_camera_status != NULL)
    {
        lv_label_set_text_fmt(s_camera_status, "CAMERA // ERROR: %s",
                              camera_control_last_error_name());
    }
}

static void camera_page_leave(Page *page)
{
    (void)page;
    if (s_camera_frame_timer != NULL)
        lv_timer_pause(s_camera_frame_timer);
    if (s_camera_capture != NULL)
        lv_obj_add_state(s_camera_capture, LV_STATE_DISABLED);
    if (camera_control_leave())
    {
        if (s_camera_status != NULL)
            lv_label_set_text(s_camera_status, "CAMERA // RELEASED");
    }
    else if (s_camera_status != NULL)
    {
        lv_label_set_text_fmt(s_camera_status, "CAMERA // RELEASE ERROR: %s",
                              camera_control_last_error_name());
    }
    (void)ui_page_destroy(PAGE_CAMERA);
}

static void camera_page_destroy(Page *page)
{
    (void)page;
    (void)camera_control_leave();
    if (s_camera_frame_timer != NULL)
    {
        lv_timer_del(s_camera_frame_timer);
        s_camera_frame_timer = NULL;
    }
    if (s_camera_pixels != NULL)
    {
        heap_caps_free(s_camera_pixels);
        s_camera_pixels = NULL;
    }
    if (s_camera_thumb_pixels != NULL)
    {
        heap_caps_free(s_camera_thumb_pixels);
        s_camera_thumb_pixels = NULL;
    }
    if (s_camera_preview_pixels != NULL)
    {
        heap_caps_free(s_camera_preview_pixels);
        s_camera_preview_pixels = NULL;
    }
    if (s_camera_decode_pixels != NULL)
    {
        heap_caps_free(s_camera_decode_pixels);
        s_camera_decode_pixels = NULL;
    }
    s_camera_image = NULL;
    s_camera_status = NULL;
    s_camera_capture = NULL;
    s_camera_resolution = NULL;
    s_camera_fps = NULL;
    s_camera_thumbnail = NULL;
    s_camera_photo_preview = NULL;
    s_camera_delete = NULL;
    s_camera_preview_container = NULL;
    s_camera_previous = NULL;
    s_camera_next = NULL;
    s_camera_preview_image = NULL;
    s_camera_last_photo_path[0] = '\0';
    s_camera_decode_busy = false;
    s_camera_decode_done = false;
    s_camera_decode_queued = false;
    s_camera_delete_status_pending = false;
}

extern "C" void camera_page_register(void)
{
    Page page{};

    page.id = PAGE_CAMERA;

    page.name = "camera";

    page.on_create = camera_page_create;

    page.on_destroy = camera_page_destroy;

    page.on_enter = camera_page_enter;

    page.on_leave = camera_page_leave;

    page.on_gesture = page_ui_back_gesture;
    ui_page_register(&page);
}
