#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>

#include <driver/ppa.h>
#include <esp_attr.h>
#include <esp_check.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <bsp/board.hpp>

/**
 * Preview data path:
 * OV2710 RAW10 -> CSI/ISP RGB565 or RGB888 -> PPA -> DPI framebuffer -> LCD.
 *
 * Board owns the camera and display hardware. This example owns the PPA client
 * and refresh semaphore, and borrows the buffers exposed by Board. All preview
 * work runs in app_main's task; the LCD ISR only signals a refresh event.
 */
namespace
{
    constexpr char kTag[] = "camera";
    constexpr std::uint32_t kDisplayWidth = bsp::Board::displayWidth;
    constexpr std::uint32_t kDisplayHeight = bsp::Board::displayHeight;
    // Keep the preview's aspect ratio on both square and portrait panels.
    // Square: 960x960 * 0.75 -> 720x720. Portrait: 480x960 * 1.5 -> 720x1440.
    constexpr std::uint32_t kPreviewCropHeight = 960;
    constexpr std::uint32_t kPreviewCropWidth =
        kDisplayWidth * kPreviewCropHeight / kDisplayHeight;
    constexpr float kPreviewScale =
        static_cast<float>(kDisplayHeight) / kPreviewCropHeight;
    // Select CAM_CTLR_COLOR_RGB565 or CAM_CTLR_COLOR_RGB888 for the preview.
    // Camera output, PPA formats, display input and buffer sizes follow it.
    constexpr cam_ctlr_color_t kCameraOutputColorType = CAM_CTLR_COLOR_RGB888;
    static_assert(kCameraOutputColorType == CAM_CTLR_COLOR_RGB565 ||
                  kCameraOutputColorType == CAM_CTLR_COLOR_RGB888);
    constexpr bool kUseRgb888 = kCameraOutputColorType == CAM_CTLR_COLOR_RGB888;
    constexpr std::size_t kPreviewBytesPerPixel = kUseRgb888 ? 3U : 2U;
    constexpr auto kPreviewPpaColorMode =
        kUseRgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    // ESP-IDF RGB888 pixels use B/G/R byte order. The Round BSP's existing
    // BGR MADCTL setting is calibrated for R/G/B framebuffer bytes (lcd_touch).
    // Normalize the camera's RGB888 bytes in PPA, preserving that panel setup.
    // The RGB565 display path performs format conversion instead of forwarding
    // three-byte pixels, so it must not inherit this RGB888-only correction.
    constexpr bool kPreviewRgbSwap = T_PANEL_P4_BSP_IS_ROUND && kUseRgb888;
    // These are LCD buffers, separate from the camera's capture buffers.
    // Three buffers add separation between PPA writes and LCD scanout.
    constexpr std::uint8_t kDisplayBufferCount = 3;
    constexpr std::size_t kDisplayBufferSize = kDisplayWidth * kDisplayHeight * kPreviewBytesPerPixel;
    constexpr std::uint32_t kFrameWaitMilliseconds = 2000;
    constexpr std::uint32_t kDisplayWaitMilliseconds = 100;
    constexpr std::int64_t kStatisticsIntervalUs = 1'000'000;

    static_assert(kPreviewCropWidth * kDisplayHeight == kPreviewCropHeight * kDisplayWidth);
    static_assert(kPreviewCropWidth % 2U == 0U);

    struct Application
    {
        // Owns sensor, CSI/ISP, panel and PSRAM buffers for the app's lifetime.
        bsp::Board board;
        pdl::camera::Ov2710ModeInfo camera_mode{};
        // Used only by the preview task, so the reusable config needs no mutex.
        ppa_client_handle_t ppa_client{};
        ppa_srm_oper_config_t ppa_config{};
        // Borrowed handles: refresh them if Board recreates the display.
        esp_lcd_panel_handle_t panel{};
        std::array<void *, kDisplayBufferCount> display_buffers{};
        std::size_t camera_frame_size{};
        // A binary semaphore coalesces periodic refresh events; it is not a
        // queue of frame completions and does not identify a particular buffer.
        SemaphoreHandle_t display_ready{};
        // DPI initially scans buffer 0; render the first preview into buffer 1.
        std::uint8_t display_buffer_index{1};
        // The next iteration must wait for a refresh after the last submission.
        bool display_refresh_pending{};
    };

    // Static lifetime keeps borrowed resources valid throughout the main loop.
    Application application;

    // ISR callback: no logging, image processing or blocking calls are allowed.
    bool IRAM_ATTR notifyDisplayRefresh(
        esp_lcd_panel_handle_t,
        esp_lcd_dpi_panel_event_data_t *,
        void *user_context)
    {
        auto semaphore = static_cast<SemaphoreHandle_t>(user_context);
        BaseType_t higher_priority_task_woken = pdFALSE;
        xSemaphoreGiveFromISR(semaphore, &higher_priority_task_woken);
        // Tell the LCD driver whether waking the preview task needs a yield.
        return higher_priority_task_woken == pdTRUE;
    }

    // Allocate BSP-owned scanout buffers and connect LCD refresh to the task.
    esp_err_t initializeDisplay(Application &app)
    {
        bsp::DisplayConfig config{};
        config.frame_buffer_count = kDisplayBufferCount;
        // This selects buffer storage, independently of the panel output depth.
        // It must match both the ISP output and PPA input/output color formats.
        config.in_color_format = kUseRgb888 ? LCD_COLOR_FMT_RGB888 : LCD_COLOR_FMT_RGB565;
        ESP_RETURN_ON_ERROR(
            app.board.initializeDisplay(config),
            kTag,
            "Display initialization failed");

        // Validate and cache these pointers once; this example never recreates
        // the display while the preview loop is running.
        app.panel = app.board.displayPanel();
        ESP_RETURN_ON_FALSE(
            app.panel != nullptr && app.board.frameBufferCount() == kDisplayBufferCount,
            ESP_ERR_INVALID_STATE, kTag, "Display buffers are unavailable");
        for (std::uint8_t index = 0; index < kDisplayBufferCount; ++index)
        {
            app.display_buffers[index] = app.board.frameBuffer(index);
            ESP_RETURN_ON_FALSE(app.display_buffers[index] != nullptr,
                                ESP_ERR_INVALID_STATE, kTag, "Display buffer %u is unavailable",
                                static_cast<unsigned int>(index));
        }

        app.display_ready = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(
            app.display_ready != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create display semaphore failed");

        esp_lcd_dpi_panel_event_callbacks_t callbacks{};
        // For a borrowed DPI framebuffer, color_trans_done runs synchronously
        // inside draw_bitmap(). Only refresh_done signals a display refresh.
        callbacks.on_refresh_done = notifyDisplayRefresh;
        return esp_lcd_dpi_panel_register_event_callbacks(
            app.panel,
            &callbacks,
            app.display_ready);
    }

    // Configure PPA's scale/rotate/mirror (SRM) engine for one blocking job at
    // a time. CSI/ISP has already converted RAW10 to the selected RGB format.
    esp_err_t initializePreviewProcessing(Application &app)
    {
        app.camera_mode = app.board.cameraModeInfo();
        ESP_RETURN_ON_FALSE(
            app.camera_mode.width >= kPreviewCropWidth &&
                app.camera_mode.height >= kPreviewCropHeight,
            ESP_ERR_NOT_SUPPORTED,
            kTag,
            "Camera mode is too small for the preview crop");

        ppa_client_config_t config{};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
        ESP_RETURN_ON_ERROR(
            ppa_register_client(&config, &app.ppa_client),
            kTag,
            "Register PPA client failed");

        app.camera_frame_size = static_cast<std::size_t>(app.camera_mode.width) *
                                app.camera_mode.height * kPreviewBytesPerPixel;
        // Geometry stays fixed; only the buffer addresses change per frame.
        auto &operation = app.ppa_config;
        // pic_w/pic_h describe the full source stride and height. block_* select
        // the centered rectangle to read from that full camera image.
        operation.in.pic_w = app.camera_mode.width;
        operation.in.pic_h = app.camera_mode.height;
        operation.in.block_w = kPreviewCropWidth;
        operation.in.block_h = kPreviewCropHeight;
        // Round the horizontal crop origin down to an even pixel position.
        operation.in.block_offset_x =
            ((app.camera_mode.width - kPreviewCropWidth) / 2U) & ~1U;
        operation.in.block_offset_y =
            (app.camera_mode.height - kPreviewCropHeight) / 2U;
        operation.in.srm_cm = kPreviewPpaColorMode;
        // PPA writes into a full LCD framebuffer, avoiding an intermediate
        // application buffer and an additional image copy before submission.
        operation.out.buffer_size = kDisplayBufferSize;
        operation.out.pic_w = kDisplayWidth;
        operation.out.pic_h = kDisplayHeight;
        operation.out.srm_cm = kPreviewPpaColorMode;
        // Hardware R/B exchange is part of the existing scale job; no CPU copy.
        operation.rgb_swap = kPreviewRgbSwap;
        operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        operation.scale_x = kPreviewScale;
        operation.scale_y = kPreviewScale;
        // The source frame can be released as soon as this blocking job returns.
        operation.mode = PPA_TRANS_MODE_BLOCKING;

        ESP_LOGI(
            kTag,
            "Preview pipeline: %ux%u %s -> crop %ux%u -> %ux%u, PPA R/B swap=%s",
            static_cast<unsigned int>(app.camera_mode.width),
            static_cast<unsigned int>(app.camera_mode.height),
            kUseRgb888 ? "RGB888" : "RGB565",
            static_cast<unsigned int>(kPreviewCropWidth),
            static_cast<unsigned int>(kPreviewCropHeight),
            static_cast<unsigned int>(kDisplayWidth),
            static_cast<unsigned int>(kDisplayHeight),
            kPreviewRgbSwap ? "on" : "off");
        return ESP_OK;
    }

    // The caller must keep source acquired and destination out of scanout reuse
    // until this operation finishes. No new allocation is needed per frame.
    esp_err_t cropAndScaleFrameToDisplay(
        Application &app,
        const std::uint8_t *source,
        void *destination)
    {
        app.ppa_config.in.buffer = source;
        app.ppa_config.out.buffer = destination;
        return ppa_do_scale_rotate_mirror(app.ppa_client, &app.ppa_config);
    }

    // Normal operation stays in this loop. Fatal errors return to app_main so
    // cleanup can stop DMA and release resources instead of leaving them active.
    esp_err_t runPreview(Application &app)
    {
        std::uint32_t submitted_frames = 0;
        std::uint32_t interval_frames = 0;
        std::uint32_t short_frames = 0;
        std::int64_t statistics_start_us = esp_timer_get_time();
        std::int64_t ppa_time_us = 0;
        std::int64_t display_wait_us = 0;
        bool backlight_enabled = false;

        while (true)
        {
            // Wait before acquiring a camera frame. Avoid holding an older
            // capture buffer or repeating PPA work while the LCD is stalled.
            if (app.display_refresh_pending)
            {
                const std::int64_t wait_start_us = esp_timer_get_time();
                ESP_RETURN_ON_FALSE(
                    xSemaphoreTake(app.display_ready,
                                   pdMS_TO_TICKS(kDisplayWaitMilliseconds)) == pdTRUE,
                    ESP_ERR_TIMEOUT, kTag, "Display refresh timed out");
                display_wait_us += esp_timer_get_time() - wait_start_us;
                app.display_refresh_pending = false;
            }

            // Acquire borrows the latest available frame, not a private copy.
            // BSP protects it from CSI overwrites until releaseCameraFrame().
            bsp::T_PANEL_P4_BSP_NAMESPACE::CameraFrame frame{};
            const esp_err_t acquire_error = app.board.acquireCameraFrame(
                frame,
                kFrameWaitMilliseconds);
            if (acquire_error == ESP_ERR_TIMEOUT)
            {
                // A temporarily missing camera frame is retryable; no frame
                // was acquired, so this path has nothing to release.
                ESP_LOGW(
                    kTag,
                    "No camera frame received for %u ms",
                    static_cast<unsigned int>(kFrameWaitMilliseconds));
                continue;
            }
            if (acquire_error == ESP_ERR_NOT_FOUND)
            {
                // CSI can reclaim the latest frame before acquisition.
                vTaskDelay(1);
                continue;
            }
            ESP_RETURN_ON_ERROR(acquire_error, kTag, "Acquire camera frame failed");

            // Reject incomplete images before PPA can read beyond valid data.
            // Even a rejected acquired frame must be returned to the BSP.
            if (frame.size < app.camera_frame_size)
            {
                ESP_LOGW(
                    kTag,
                    "Short camera frame: %u of %u bytes",
                    static_cast<unsigned int>(frame.size),
                    static_cast<unsigned int>(app.camera_frame_size));
                ++short_frames;
                ESP_RETURN_ON_ERROR(app.board.releaseCameraFrame(frame.data),
                                    kTag, "Release short camera frame failed");
                continue;
            }

            void *display_buffer =
                app.display_buffers[app.display_buffer_index];
            const std::int64_t ppa_start_us = esp_timer_get_time();
            const esp_err_t crop_error =
                cropAndScaleFrameToDisplay(app, frame.data, display_buffer);
            ppa_time_us += esp_timer_get_time() - ppa_start_us;
            // Blocking PPA has finished reading the capture buffer. Release
            // it even on a PPA error, before any display operation.
            ESP_RETURN_ON_ERROR(app.board.releaseCameraFrame(frame.data),
                                kTag, "Release camera frame failed");
            ESP_RETURN_ON_ERROR(crop_error, kTag, "PPA camera crop failed");

            // A BSP DPI framebuffer selects the panel's next scanout buffer.
            // The call submits pixels; it does not wait for visible scanout.
            const esp_err_t draw_error = esp_lcd_panel_draw_bitmap(
                app.panel,
                0,
                0,
                kDisplayWidth,
                kDisplayHeight,
                display_buffer);
            ESP_RETURN_ON_ERROR(draw_error, kTag, "Display camera frame failed");
            // Discard any periodic refresh from before (or during) submission.
            // Require a future refresh before the next buffer rotation.
            (void)xSemaphoreTake(app.display_ready, 0);
            app.display_refresh_pending = true;

            // Advance the candidate now, but gate its use on the refresh wait
            // at the top of the next iteration. The order is 1 -> 2 -> 0 -> 1.
            app.display_buffer_index =
                (app.display_buffer_index + 1U) % kDisplayBufferCount;
            ++submitted_frames;
            ++interval_frames;
            if (!backlight_enabled)
            {
                // Keep startup allocations and unsubmitted pixels hidden.
                ESP_RETURN_ON_ERROR(app.board.setBacklightBrightness(80),
                                    kTag, "Enable backlight failed");
                backlight_enabled = true;
            }

            // Report roughly once per second, independently of sensor FPS.
            // FPS counts submissions; PPA/LCD timings are interval averages.
            // BSP received/dropped and local submitted/short counts are totals.
            const std::int64_t now_us = esp_timer_get_time();
            if (now_us - statistics_start_us >= kStatisticsIntervalUs)
            {
                const float fps =
                    static_cast<float>(interval_frames) * 1'000'000.0F /
                    static_cast<float>(now_us - statistics_start_us);
                const auto statistics = app.board.cameraStatistics();
                ESP_LOGI(
                    kTag,
                    "Preview %.1f FPS, PPA=%.2f ms/frame, LCD wait=%.2f ms/frame"
                    ", received=%" PRIu32 ", submitted=%" PRIu32
                    ", dropped=%" PRIu32 ", short=%" PRIu32,
                    static_cast<double>(fps),
                    static_cast<double>(ppa_time_us) / interval_frames / 1000.0,
                    static_cast<double>(display_wait_us) / interval_frames / 1000.0,
                    statistics.received_frames,
                    submitted_frames,
                    statistics.dropped_frames,
                    short_frames);
                statistics_start_us = now_us;
                interval_frames = 0;
                ppa_time_us = 0;
                display_wait_us = 0;
            }
        }
    }

    // Prepare every consumer before enabling CSI capture. This avoids starting
    // a stream while the display or PPA is still being initialized.
    esp_err_t initializePreview(Application &app)
    {
        ESP_RETURN_ON_ERROR(initializeDisplay(app), kTag, "Initialize display failed");
        bsp::T_PANEL_P4_BSP_NAMESPACE::CameraConfig config{};
        config.mode = pdl::camera::Ov2710Mode::raw10_1920x1080_25fps;
        config.input_data_color_type = CAM_CTLR_COLOR_RAW10;
        config.output_data_color_type = kCameraOutputColorType;
        config.auto_exposure_target = 0x28;
        ESP_RETURN_ON_ERROR(app.board.initializeCamera(config), kTag, "Initialize camera failed");
        ESP_RETURN_ON_ERROR(initializePreviewProcessing(app), kTag, "Initialize PPA failed");
        return app.board.startCameraStreaming();
    }

    // Also handles partially completed initialization. Continue independent
    // cleanup steps if one fails, while logging each error for diagnosis.
    void cleanupPreview(Application &app)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(app.board.deinitializeCamera());
        if (app.ppa_client != nullptr)
        {
            const esp_err_t error = ppa_unregister_client(app.ppa_client);
            ESP_ERROR_CHECK_WITHOUT_ABORT(error);
            if (error == ESP_OK)
                app.ppa_client = nullptr;
        }
        // Stop the panel ISR before deleting the semaphore it can signal.
        const esp_err_t error = app.board.deinitializeDisplay();
        ESP_ERROR_CHECK_WITHOUT_ABORT(error);
        if (error == ESP_OK && app.display_ready != nullptr)
        {
            vSemaphoreDelete(app.display_ready);
            app.display_ready = nullptr;
        }
        // If panel teardown failed, retain its semaphore: an ISR may still use it.
        app.panel = nullptr;
        app.display_buffers.fill(nullptr);
    }
}

// ESP-IDF calls this C-linkage entry point from the main FreeRTOS task.
extern "C" void app_main(void)
{
    esp_err_t error = initializePreview(application);
    if (error == ESP_OK)
    {
        ESP_LOGI(kTag, "OV2710 camera preview started");
        error = runPreview(application);
    }
    ESP_LOGE(kTag, "Camera preview stopped: %s", esp_err_to_name(error));
    cleanupPreview(application);
}
