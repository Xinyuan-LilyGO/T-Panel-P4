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

#if T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_STANDARD
#include <bsp/t_panel_p4_standard.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE T_PANEL_P4_FPS_BOARD_NAMESPACE
#elif T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_ROUND
#include <bsp/t_panel_p4_round.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE t_panel_p4_round
#elif T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_RECT
#include <bsp/t_panel_p4_rect.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE t_panel_p4_rect
#else
#error "Unsupported T_PANEL_P4_HARDWARE_VERSION"
#endif


namespace
{
    constexpr char kTag[] = "camera";
    constexpr std::uint32_t kDisplayWidth = T_PANEL_P4_FPS_BOARD_NAMESPACE::Board::displayWidth;
    constexpr std::uint32_t kDisplayHeight = T_PANEL_P4_FPS_BOARD_NAMESPACE::Board::displayHeight;
    constexpr std::uint32_t kPreviewCropSize = 960;
    constexpr float kPreviewScale = 0.75F;
    constexpr std::size_t kRgb565BytesPerPixel = 2;
    constexpr std::uint8_t kDisplayBufferCount = 3;
    constexpr std::size_t kDisplayBufferSize = kDisplayWidth * kDisplayHeight * kRgb565BytesPerPixel;
    constexpr std::uint32_t kFrameWaitMilliseconds = 2000;
    constexpr std::uint32_t kDisplayWaitMilliseconds = 100;
    constexpr std::uint32_t kStatisticsFrameInterval = 25;

    static_assert(kPreviewCropSize * 3U / 4U == kDisplayWidth);
    static_assert(kDisplayWidth == kDisplayHeight);

    struct Application
    {
        bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::Board board;
        pdl::camera::Ov2710ModeInfo camera_mode{};
        ppa_client_handle_t ppa_client{};
        SemaphoreHandle_t display_ready{};
        std::uint8_t display_buffer_index{1};
        bool display_transfer_pending{};
    };

    Application application;

    bool IRAM_ATTR finishDisplay(
        esp_lcd_panel_handle_t,
        esp_lcd_dpi_panel_event_data_t *,
        void *user_context)
    {
        auto *app = static_cast<Application *>(user_context);
        BaseType_t higher_priority_task_woken = pdFALSE;
        xSemaphoreGiveFromISR(app->display_ready, &higher_priority_task_woken);
        return higher_priority_task_woken == pdTRUE;
    }

    esp_err_t initializeDisplay(Application &app)
    {
        bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::DisplayConfig config{};
        config.frame_buffer_count = kDisplayBufferCount;
        ESP_RETURN_ON_ERROR(
            app.board.initializeDisplay(config),
            kTag,
            "Display initialization failed");

        app.display_ready = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(
            app.display_ready != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create display semaphore failed");

        esp_lcd_dpi_panel_event_callbacks_t callbacks{};
        callbacks.on_color_trans_done = finishDisplay;
        return esp_lcd_dpi_panel_register_event_callbacks(
            app.board.displayPanel(),
            &callbacks,
            &app);
    }

    esp_err_t initializePreviewProcessing(Application &app)
    {
        app.camera_mode = app.board.cameraModeInfo();
        ESP_RETURN_ON_FALSE(
            app.camera_mode.width >= kPreviewCropSize &&
                app.camera_mode.height >= kPreviewCropSize,
            ESP_ERR_NOT_SUPPORTED,
            kTag,
            "Camera mode is too small for the 960x960 preview crop");

        ppa_client_config_t config{};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        config.data_burst_length = PPA_DATA_BURST_LENGTH_128;
        ESP_RETURN_ON_ERROR(
            ppa_register_client(&config, &app.ppa_client),
            kTag,
            "Register PPA client failed");

        ESP_LOGI(
            kTag,
            "Preview pipeline: %ux%u RGB565 -> crop %ux%u -> %ux%u",
            static_cast<unsigned int>(app.camera_mode.width),
            static_cast<unsigned int>(app.camera_mode.height),
            static_cast<unsigned int>(kPreviewCropSize),
            static_cast<unsigned int>(kPreviewCropSize),
            static_cast<unsigned int>(kDisplayWidth),
            static_cast<unsigned int>(kDisplayHeight));
        return ESP_OK;
    }

    esp_err_t cropAndScaleFrameToDisplay(
        Application &app,
        const std::uint8_t *source,
        void *destination)
    {
        const std::uint32_t crop_x =
            (app.camera_mode.width - kPreviewCropSize) / 2U;
        const std::uint32_t crop_y =
            (app.camera_mode.height - kPreviewCropSize) / 2U;

        ppa_srm_oper_config_t config{};
        config.in.buffer = source;
        config.in.pic_w = app.camera_mode.width;
        config.in.pic_h = app.camera_mode.height;
        config.in.block_w = kPreviewCropSize;
        config.in.block_h = kPreviewCropSize;
        config.in.block_offset_x = crop_x;
        config.in.block_offset_y = crop_y;
        config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        config.out.buffer = destination;
        config.out.buffer_size = kDisplayBufferSize;
        config.out.pic_w = kDisplayWidth;
        config.out.pic_h = kDisplayHeight;
        config.out.block_offset_x = 0;
        config.out.block_offset_y = 0;
        config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        config.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        config.scale_x = kPreviewScale;
        config.scale_y = kPreviewScale;
        config.mode = PPA_TRANS_MODE_BLOCKING;
        return ppa_do_scale_rotate_mirror(app.ppa_client, &config);
    }

    void runPreview(Application &app)
    {
        const std::size_t expected_frame_size =
            static_cast<std::size_t>(app.camera_mode.width) *
            app.camera_mode.height * kRgb565BytesPerPixel;
        std::uint32_t displayed_frames = 0;
        std::int64_t statistics_start_us = esp_timer_get_time();
        bool backlight_enabled = false;

        while (true)
        {
            bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::CameraFrame frame{};
            const esp_err_t acquire_error = app.board.acquireCameraFrame(
                frame,
                kFrameWaitMilliseconds);
            if (acquire_error == ESP_ERR_TIMEOUT)
            {
                ESP_LOGW(
                    kTag,
                    "No camera frame received for %u ms",
                    static_cast<unsigned int>(kFrameWaitMilliseconds));
                continue;
            }
            if (acquire_error != ESP_OK)
            {
                ESP_LOGE(
                    kTag,
                    "Acquire camera frame failed: %s",
                    esp_err_to_name(acquire_error));
                continue;
            }

            if (frame.size < expected_frame_size)
            {
                ESP_LOGW(
                    kTag,
                    "Short camera frame: %u of %u bytes",
                    static_cast<unsigned int>(frame.size),
                    static_cast<unsigned int>(expected_frame_size));
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    app.board.releaseCameraFrame(frame.data));
                continue;
            }

            void *display_buffer =
                app.board.frameBuffer(app.display_buffer_index);
            if (display_buffer == nullptr)
            {
                ESP_LOGE(
                    kTag,
                    "Display frame buffer %u is unavailable (count=%u)",
                    static_cast<unsigned int>(app.display_buffer_index),
                    static_cast<unsigned int>(app.board.frameBufferCount()));
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    app.board.releaseCameraFrame(frame.data));
                return;
            }
            const esp_err_t crop_error =
                cropAndScaleFrameToDisplay(app, frame.data, display_buffer);
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                app.board.releaseCameraFrame(frame.data));
            if (crop_error != ESP_OK)
            {
                ESP_LOGE(
                    kTag,
                    "PPA camera crop failed: %s",
                    esp_err_to_name(crop_error));
                continue;
            }

            if (app.display_transfer_pending)
            {
                if (xSemaphoreTake(
                        app.display_ready,
                        pdMS_TO_TICKS(kDisplayWaitMilliseconds)) != pdTRUE)
                {
                    ESP_LOGW(kTag, "Display frame synchronization timed out");
                    continue;
                }
                app.display_transfer_pending = false;
            }

            const esp_err_t draw_error = esp_lcd_panel_draw_bitmap(
                app.board.displayPanel(),
                0,
                0,
                kDisplayWidth,
                kDisplayHeight,
                display_buffer);
            if (draw_error != ESP_OK)
            {
                ESP_LOGE(
                    kTag,
                    "Display camera frame failed: %s",
                    esp_err_to_name(draw_error));
                continue;
            }
            app.display_transfer_pending = true;

            app.display_buffer_index =
                (app.display_buffer_index + 1U) % kDisplayBufferCount;
            ++displayed_frames;
            if (!backlight_enabled)
            {
                ESP_ERROR_CHECK(app.board.setBacklightBrightness(80));
                backlight_enabled = true;
            }

            if (displayed_frames % kStatisticsFrameInterval == 0U)
            {
                const std::int64_t now_us = esp_timer_get_time();
                const float fps =
                    static_cast<float>(kStatisticsFrameInterval) * 1'000'000.0F /
                    static_cast<float>(now_us - statistics_start_us);
                statistics_start_us = now_us;
                const auto statistics = app.board.cameraStatistics();
                ESP_LOGI(
                    kTag,
                    "Preview %.1f FPS, received=%" PRIu32
                    ", displayed=%" PRIu32 ", dropped=%" PRIu32,
                    static_cast<double>(fps),
                    statistics.received_frames,
                    displayed_frames,
                    statistics.dropped_frames);
            }
        }
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(application.board.initialize());
    ESP_ERROR_CHECK(initializeDisplay(application));

    bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::CameraConfig camera_config{};
    camera_config.mode = pdl::camera::Ov2710Mode::raw10_1920x1080_25fps;
    camera_config.capture_buffer_count = 3;
    camera_config.auto_exposure_target = 0x28;
    camera_config.isp_clock_hz = 160'000'000;
    camera_config.allow_non_dma_psram_fallback = true;
    ESP_ERROR_CHECK(application.board.initializeCamera(camera_config));
    ESP_ERROR_CHECK(initializePreviewProcessing(application));

    ESP_LOGI(kTag, "OV2710 camera preview started");
    runPreview(application);
}
