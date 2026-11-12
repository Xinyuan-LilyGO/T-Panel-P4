#include "page_common.hpp"
#include "page_registry.hpp"
#include <ui/core/lvgl_port.hpp>

#include <bsp/board.hpp>
#include <driver/ppa.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace factory::ui
{
namespace
{
constexpr char kTag[] = "factory_round_camera";
constexpr lv_coord_t kScreenSize = 720;
constexpr uint32_t kFramePeriodMs = 40;
// 1/2 is exactly representable by PPA. Both rectangles have aspect 15:11,
// so this fills 720x528 without distortion or a rounded-off output edge.
constexpr uint32_t kCropWidth = 1440;
constexpr uint32_t kCropHeight = 1056;
constexpr float kPreviewScale = 0.5F;
constexpr size_t kPreviewPixels = static_cast<size_t>(kScreenSize) * kScreenSize;
constexpr size_t kRgb888BytesPerPixel = 3;
constexpr size_t kPreviewBytes = kPreviewPixels * kRgb888BytesPerPixel;
constexpr lv_coord_t kThumbnailSize = 64;
constexpr uint32_t kPreviewTop = 64;
constexpr uint32_t kPreviewHeight = 528;
static_assert(kCropWidth / 2U == kScreenSize && kCropWidth % 2U == 0U);
static_assert(kCropHeight / 2U == kPreviewHeight && kCropHeight % 2U == 0U);
constexpr size_t kFramebufferBytes = kPreviewPixels * kRgb888BytesPerPixel;

struct CameraUi
{
    lv_obj_t *image{};
    lv_obj_t *status{};
    lv_obj_t *fps{};
    lv_obj_t *shutter{};
    lv_obj_t *thumbnail_button{};
    lv_obj_t *thumbnail{};
    lv_obj_t *thumbnail_placeholder{};
    lv_obj_t *delete_button{};
    lv_obj_t *photo_view{};
    lv_timer_t *timer{};
    ppa_client_handle_t ppa_client{};
};

CameraUi s_ui{};
bsp::t_panel_p4_round::Board *s_board{};
uint8_t *s_pixels{};
bool s_camera_active{};
uint8_t *s_photo_pixels{};
lv_image_dsc_t s_photo_dsc{};
uint8_t *s_thumbnail_pixels{};
lv_image_dsc_t s_thumbnail_dsc{};
bool s_has_photo{};
int64_t s_fps_start_us{};
int64_t s_last_frame_us{};
int64_t s_stream_start_us{};
uint32_t s_frame_count{};
uint32_t s_ppa_count{};
int64_t s_ppa_window_us{};
uint8_t *s_last_framebuffer{};
const char *s_pending_error{};

void showError(const char *message)
{
    if (s_ui.status == nullptr)
        return;
    if (message == nullptr && lv_obj_has_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN))
        return;
    if (message != nullptr && !lv_obj_has_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN) &&
        std::strcmp(lv_label_get_text(s_ui.status), message) == 0)
        return;
    lv_label_set_text(s_ui.status, message != nullptr ? message : "");
    if (message != nullptr)
        lv_obj_remove_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_ui.status, LV_OBJ_FLAG_HIDDEN);
}

void capturePhoto(lv_event_t *)
{
    if (!s_camera_active || s_pixels == nullptr || s_photo_pixels == nullptr ||
        s_thumbnail_pixels == nullptr ||
        s_last_frame_us == 0)
        return;
    // Preserve the full preview in PSRAM independently of the live buffer.
    // The small thumbnail avoids rescaling the full photo on every UI redraw.
    if (s_last_framebuffer != nullptr)
    {
        const auto *framebuffer = s_last_framebuffer;
        for (uint32_t y = 0; y < kScreenSize; ++y)
            std::memcpy(s_photo_pixels + static_cast<size_t>(y) * kScreenSize * 3U,
                        framebuffer + static_cast<size_t>(kPreviewTop +
                            y * kPreviewHeight / kScreenSize) * kScreenSize * 3U,
                        kScreenSize * 3U);
    }
    else
        std::memcpy(s_photo_pixels, s_pixels, kPreviewBytes);
    for (uint32_t y = 0; y < kThumbnailSize; ++y)
        for (uint32_t x = 0; x < kThumbnailSize; ++x)
        {
            const uint32_t source_index =
                ((y * kScreenSize / kThumbnailSize) * kScreenSize +
                 x * kScreenSize / kThumbnailSize) * kRgb888BytesPerPixel;
            const uint32_t destination_index =
                (y * kThumbnailSize + x) * kRgb888BytesPerPixel;
            std::memcpy(s_thumbnail_pixels + destination_index,
                        s_photo_pixels + source_index, kRgb888BytesPerPixel);
        }
    s_has_photo = true;
    lv_image_set_src(s_ui.thumbnail, &s_thumbnail_dsc);
    lv_image_set_src(s_ui.photo_view, &s_photo_dsc);
    lv_obj_invalidate(s_ui.thumbnail);
    lv_obj_add_flag(s_ui.thumbnail_placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(s_ui.delete_button, LV_STATE_DISABLED);
}

void togglePhotoPreview(lv_event_t *)
{
    if (!s_has_photo)
        return;
    if (lv_obj_has_flag(s_ui.photo_view, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(s_ui.photo_view, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_ui.photo_view, LV_OBJ_FLAG_HIDDEN);
}

void deletePhoto(lv_event_t *)
{
    if (!s_has_photo)
        return;
    s_has_photo = false;
    lv_obj_add_flag(s_ui.photo_view, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_ui.photo_view, nullptr);
    lv_image_set_src(s_ui.thumbnail, nullptr);
    lv_obj_remove_flag(s_ui.thumbnail_placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(s_ui.delete_button, LV_STATE_DISABLED);
}

lv_image_dsc_t s_frame_dsc{};

void renderCameraFrame(uint8_t *display_buffer)
{
    if (!s_camera_active || s_board == nullptr || s_pixels == nullptr || s_ui.image == nullptr)
        return;
    if (!lv_obj_has_flag(s_ui.photo_view, LV_OBJ_FLAG_HIDDEN))
        return;
    const int64_t now = esp_timer_get_time();
    bsp::t_panel_p4_round::CameraFrame frame{};
    const esp_err_t result = s_board->acquireCameraFrame(frame, 0U);
    if (result != ESP_OK || frame.data == nullptr)
    {
        // DIRECT mode alternates two buffers. If capture has no fresh frame,
        // retain the most recently displayed video instead of showing the
        // older video still present in this destination buffer.
        if (s_last_framebuffer != nullptr && s_last_framebuffer != display_buffer)
        {
            constexpr size_t offset = kPreviewTop * kScreenSize * kRgb888BytesPerPixel;
            constexpr size_t bytes = kPreviewHeight * kScreenSize * kRgb888BytesPerPixel;
            std::memcpy(display_buffer + offset, s_last_framebuffer + offset, bytes);
        }
        const int64_t last_frame = s_last_frame_us != 0 ? s_last_frame_us : s_stream_start_us;
        if (result != ESP_ERR_TIMEOUT || now - last_frame >= 2'000'000)
        {
            s_pending_error = "Camera frame unavailable";

        }
        return;
    }
    const auto mode = s_board->cameraModeInfo();
    const uint32_t source_width = mode.width;
    const uint32_t source_height = mode.height;
    const size_t required = static_cast<size_t>(source_width) * source_height * 2U;
    if (source_width == 0U || source_height == 0U || frame.size < required)
    {
        (void)s_board->releaseCameraFrame(frame.data);
        s_pending_error = "Invalid camera frame";
        return;
    }
    if (s_ui.ppa_client == nullptr)
    {
        (void)s_board->releaseCameraFrame(frame.data);
        s_pending_error = "PPA scaler unavailable";
        return;
    }
    if (source_width < kCropWidth || source_height < kCropHeight)
    {
        (void)s_board->releaseCameraFrame(frame.data);
        s_pending_error = "Camera mode too small for PPA crop";
        return;
    }
    const uint32_t crop_x = (source_width - kCropWidth) / 2U;

    ppa_srm_oper_config_t ppa_config{};
    ppa_config.in.buffer = frame.data;
    ppa_config.in.pic_w = source_width;
    ppa_config.in.pic_h = source_height;
    ppa_config.in.block_w = kCropWidth;
    ppa_config.in.block_h = kCropHeight;
    ppa_config.in.block_offset_x = crop_x;
    ppa_config.in.block_offset_y = (source_height - kCropHeight) / 2U;
    ppa_config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    if (display_buffer == nullptr)
    {
        (void)s_board->releaseCameraFrame(frame.data);
        s_pending_error = "Display framebuffer unavailable";
        return;
    }
    ppa_config.out.buffer = display_buffer;
    ppa_config.out.buffer_size = kFramebufferBytes;
    ppa_config.out.pic_w = kScreenSize;
    ppa_config.out.pic_h = kScreenSize;
    ppa_config.out.block_offset_x = 0;
    ppa_config.out.block_offset_y = kPreviewTop;
    ppa_config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
    ppa_config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    ppa_config.scale_x = kPreviewScale;
    ppa_config.scale_y = kPreviewScale;
    ppa_config.mode = PPA_TRANS_MODE_BLOCKING;
    const int64_t ppa_start_us = esp_timer_get_time();
    const esp_err_t ppa_result = ppa_do_scale_rotate_mirror(s_ui.ppa_client, &ppa_config);
    const int64_t ppa_elapsed_us = esp_timer_get_time() - ppa_start_us;
    ++s_ppa_count;
    if (s_ppa_window_us == 0)
        s_ppa_window_us = esp_timer_get_time();
    if (esp_timer_get_time() - s_ppa_window_us >= 1'000'000)
    {
        ESP_LOGI(kTag, "PPA: %u frames/s, last=%lld us",
                 static_cast<unsigned>(s_ppa_count),
                 static_cast<long long>(ppa_elapsed_us));
        s_ppa_count = 0;
        s_ppa_window_us = esp_timer_get_time();
    }
    if (ppa_result != ESP_OK)
    {
        (void)s_board->releaseCameraFrame(frame.data);
        s_pending_error = "PPA frame scaling failed";
        return;
    }
    (void)s_board->releaseCameraFrame(frame.data);
    s_last_framebuffer = display_buffer;
    s_last_frame_us = esp_timer_get_time();
    ++s_frame_count;
    s_pending_error = nullptr;

}

void updateFrame(lv_timer_t *)
{
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - s_fps_start_us;
    if (elapsed >= 1'000'000)
    {
        const uint32_t fps_tenths = static_cast<uint32_t>(
            static_cast<uint64_t>(s_frame_count) * 10'000'000ULL / elapsed);
        lv_label_set_text_fmt(s_ui.fps, "%u.%u FPS",
                              static_cast<unsigned>(fps_tenths / 10U),
                              static_cast<unsigned>(fps_tenths % 10U));
        s_frame_count = 0;
        s_fps_start_us = now;
    }
    showError(s_pending_error);
    if (s_last_frame_us != 0 && s_pending_error == nullptr)
        lv_obj_remove_state(s_ui.shutter, LV_STATE_DISABLED);
    else
        lv_obj_add_state(s_ui.shutter, LV_STATE_DISABLED);

    if (s_camera_active && lv_obj_has_flag(s_ui.photo_view, LV_OBJ_FLAG_HIDDEN))
        // Only a small UI region is redrawn. The pre-flush hook writes video
        // into the same inactive buffer before LVGL submits it to the panel.
        lv_obj_invalidate(s_ui.fps);
}

lv_obj_t *cameraButton(lv_obj_t *parent, const char *symbol, lv_coord_t size)
{
    lv_obj_t *button = lv_obj_create(parent);
    if (button == nullptr)
        return nullptr;
    lv_obj_set_size(button, size, size);
    page_ui_apply_icon_style(button);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_t *icon = page_ui_label(button, symbol, &lv_font_montserrat_24, UI_WIHTE);
    if (icon != nullptr)
    {
        lv_obj_center(icon);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    }
    return button;
}

void cameraPageCreate(Page *page)
{
    if (page == nullptr || page->root == nullptr)
        return;
    lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(page->root, 0, LV_PART_MAIN);
    lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(page->root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(page->root, LV_OPA_COVER, LV_PART_MAIN);
    s_pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        128, kPreviewBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_ui.image = lv_image_create(page->root);
    lv_obj_set_size(s_ui.image, kScreenSize, kScreenSize);
    lv_obj_set_pos(s_ui.image, 0, 0);
    lv_obj_remove_flag(s_ui.image, LV_OBJ_FLAG_SCROLLABLE);
    // The live image is written directly by PPA into the display framebuffer.
    lv_obj_add_flag(s_ui.image, LV_OBJ_FLAG_HIDDEN);
    if (s_pixels != nullptr)
    {
        std::memset(s_pixels, 0, kPreviewBytes);
        s_frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_frame_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
        s_frame_dsc.header.w = kScreenSize;
        s_frame_dsc.header.h = kScreenSize;
        s_frame_dsc.header.stride = kScreenSize * kRgb888BytesPerPixel;
        s_frame_dsc.data_size = kPreviewBytes;
        s_frame_dsc.data = reinterpret_cast<const uint8_t *>(s_pixels);
        lv_image_set_src(s_ui.image, &s_frame_dsc);
    }
    s_ui.status = page_ui_label(page->root, "", &lv_font_montserrat_16, UI_WIHTE);
    if (s_ui.status != nullptr)
    {
        lv_obj_set_width(s_ui.status, 560);
        lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(s_ui.status, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_ui.status, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(s_ui.status, 12, 0);
        lv_obj_align(s_ui.status, LV_ALIGN_CENTER, 0, 0);
        showError(nullptr);
    }
    s_ui.fps = page_ui_label(page->root, "0.0 FPS", &lv_font_montserrat_20, UI_WIHTE);
    lv_obj_set_size(s_ui.fps, 180, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_ui.fps, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ui.fps, (kScreenSize - 180) / 2, 24);

    s_ui.shutter = cameraButton(page->root, LV_SYMBOL_IMAGE, 88);
    lv_obj_set_pos(s_ui.shutter, (kScreenSize - 88) / 2, kScreenSize - 112);
    lv_obj_add_state(s_ui.shutter, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_ui.shutter, capturePhoto, LV_EVENT_CLICKED, nullptr);

    s_ui.thumbnail_button = cameraButton(page->root, LV_SYMBOL_IMAGE, kThumbnailSize);
    lv_obj_add_event_cb(s_ui.thumbnail_button, togglePhotoPreview, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_pos(s_ui.thumbnail_button, 208, kScreenSize - 100);
    lv_obj_set_style_clip_corner(s_ui.thumbnail_button, true, 0);
    s_ui.thumbnail_placeholder = lv_obj_get_child(s_ui.thumbnail_button, 0);
    s_ui.thumbnail = lv_image_create(s_ui.thumbnail_button);
    lv_obj_set_size(s_ui.thumbnail, kThumbnailSize, kThumbnailSize);
    lv_obj_center(s_ui.thumbnail);
    lv_obj_remove_flag(s_ui.thumbnail, LV_OBJ_FLAG_CLICKABLE);
    s_ui.delete_button = cameraButton(page->root, LV_SYMBOL_TRASH, kThumbnailSize);
    lv_obj_set_pos(s_ui.delete_button, 448, kScreenSize - 100);
    lv_obj_add_state(s_ui.delete_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_ui.delete_button, deletePhoto, LV_EVENT_CLICKED, nullptr);

    s_photo_pixels = static_cast<uint8_t *>(heap_caps_malloc(
        kPreviewBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_thumbnail_pixels = static_cast<uint8_t *>(heap_caps_malloc(
        kThumbnailSize * kThumbnailSize * kRgb888BytesPerPixel,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_photo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf = LV_COLOR_FORMAT_RGB888;
    s_photo_dsc.header.w = kScreenSize;
    s_photo_dsc.header.h = kScreenSize;
    s_photo_dsc.header.stride = kScreenSize * kRgb888BytesPerPixel;
    s_photo_dsc.data_size = kPreviewBytes;
    s_photo_dsc.data = reinterpret_cast<const uint8_t *>(s_photo_pixels);
    s_thumbnail_dsc = s_photo_dsc;
    s_thumbnail_dsc.header.w = kThumbnailSize;
    s_thumbnail_dsc.header.h = kThumbnailSize;
    s_thumbnail_dsc.header.stride = kThumbnailSize * kRgb888BytesPerPixel;
    s_thumbnail_dsc.data_size = kThumbnailSize * kThumbnailSize * kRgb888BytesPerPixel;
    s_thumbnail_dsc.data = reinterpret_cast<const uint8_t *>(s_thumbnail_pixels);

    // Created last so the photo covers the live image, FPS and controls.
    s_ui.photo_view = lv_image_create(page->root);
    lv_obj_set_size(s_ui.photo_view, kScreenSize, kScreenSize);
    lv_obj_set_pos(s_ui.photo_view, 0, 0);
    lv_obj_remove_flag(s_ui.photo_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.photo_view, static_cast<lv_obj_flag_t>(
        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_HIDDEN));
    lv_obj_add_event_cb(s_ui.photo_view, togglePhotoPreview, LV_EVENT_CLICKED, nullptr);
    s_ui.timer = lv_timer_create(updateFrame, kFramePeriodMs, nullptr);
    if (s_ui.timer != nullptr)
        lv_timer_pause(s_ui.timer);
    ppa_client_config_t ppa_config{};
    ppa_config.oper_type = PPA_OPERATION_SRM;
    ppa_config.max_pending_trans_num = 1;
    ppa_config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
    if (ppa_register_client(&ppa_config, &s_ui.ppa_client) != ESP_OK)
        s_ui.ppa_client = nullptr;
}

void cameraPageEnter(Page *)
{
    if (s_board == nullptr)
    {
        showError("Camera board unavailable");
        return;
    }
    if (s_pixels == nullptr || s_photo_pixels == nullptr ||
        s_thumbnail_pixels == nullptr || s_ui.timer == nullptr)
    {
        showError("Camera preview: insufficient memory");
        return;
    }
    if (s_ui.ppa_client == nullptr)
    {
        showError("PPA scaler initialization failed");
        return;
    }
    esp_err_t result = ESP_OK;
    if (s_board->isCameraInitialized())
    {
        auto camera_config = s_board->cameraConfig();
        if (camera_config.mode != pdl::camera::Ov2710Mode::raw10_1920x1080_25fps)
        {
            camera_config.mode = pdl::camera::Ov2710Mode::raw10_1920x1080_25fps;
            result = s_board->reconfigureCamera(camera_config);
        }
        if (result == ESP_OK)
            result = s_board->startCameraStreaming();
    }
    else
    {
        bsp::t_panel_p4_round::CameraConfig camera_config{};
        camera_config.mode = pdl::camera::Ov2710Mode::raw10_1920x1080_25fps;
        camera_config.capture_buffer_count = 3;
        camera_config.auto_exposure_target = 0x28;
        camera_config.isp_clock_hz = 160'000'000;
        camera_config.allow_non_dma_psram_fallback = true;
        result = s_board->initializeCamera(camera_config);
        if (result == ESP_OK)
            result = s_board->startCameraStreaming();
    }
    s_camera_active = result == ESP_OK;
    if (s_camera_active)
    {
        ESP_LOGI(kTag, "Preview: 1920x1080 RGB565 -> center crop 1440x1056"
                      " at (240,12) -> scale 1/2 -> 720x528 RGB888");
        setFramebufferOverlay(renderCameraFrame);
    }
    showError(s_camera_active ? nullptr : "Camera initialization failed");
    s_frame_count = 0;
    s_ppa_count = 0;
    s_ppa_window_us = esp_timer_get_time();
    s_fps_start_us = esp_timer_get_time();
    s_stream_start_us = s_fps_start_us;
    s_last_frame_us = 0;
    if (s_ui.timer != nullptr)
    {
        if (s_camera_active)
            lv_timer_resume(s_ui.timer);
        else
            lv_timer_pause(s_ui.timer);
    }
    if (!s_camera_active)
        ESP_LOGE(kTag, "camera initialization failed: %s", esp_err_to_name(result));
}

void cameraPageLeave(Page *)
{
    setFramebufferOverlay(nullptr);
    s_camera_active = false;
    if (s_ui.timer != nullptr)
        lv_timer_pause(s_ui.timer);
    if (s_board != nullptr && s_board->isCameraInitialized())
        (void)s_board->deinitializeCamera();
    (void)ui_page_destroy(PAGE_CAMERA);
}

void cameraPageDestroy(Page *)
{
    setFramebufferOverlay(nullptr);
    s_last_framebuffer = nullptr;
    s_pending_error = nullptr;
    s_camera_active = false;
    if (s_ui.timer != nullptr)
    {
        lv_timer_del(s_ui.timer);
        s_ui.timer = nullptr;
    }
    if (s_ui.ppa_client != nullptr)
    {
        (void)ppa_unregister_client(s_ui.ppa_client);
        s_ui.ppa_client = nullptr;
    }
    if (s_pixels != nullptr)
    {
        heap_caps_free(s_pixels);
        s_pixels = nullptr;
    }
    heap_caps_free(s_photo_pixels);
    s_photo_pixels = nullptr;
    heap_caps_free(s_thumbnail_pixels);
    s_thumbnail_pixels = nullptr;
    s_has_photo = false;
    s_frame_dsc = {};
    s_photo_dsc = {};
    s_thumbnail_dsc = {};
    s_ui = {};
}
}

void setCameraBoard(bsp::t_panel_p4_round::Board *board) { s_board = board; }

void registerCameraPage()
{
    Page page{};
    page.id = PAGE_CAMERA;
    page.name = "camera";
    page.on_create = cameraPageCreate;
    page.on_destroy = cameraPageDestroy;
    page.on_enter = cameraPageEnter;
    page.on_leave = cameraPageLeave;
    page.on_gesture = page_ui_back_gesture;
    (void)ui_page_register(&page);
}
}

extern "C" void camera_page_register() { factory::ui::registerCameraPage(); }



