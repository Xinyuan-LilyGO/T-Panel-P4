#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <demos/lv_demos.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <pdl/core/status.hpp>
#include <pdl/drivers/touch/jadard/jadard.hpp>

namespace
{
    static_assert(
        LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5 && LVGL_VERSION_PATCH == 0,
        "This example requires LVGL 9.5.0");
    static_assert(LV_USE_OS == LV_OS_FREERTOS, "LVGL FreeRTOS backend is required");
    static_assert(LV_DRAW_SW_DRAW_UNIT_CNT == 2, "Two software draw units are required");
    static_assert(LV_OBJ_STYLE_CACHE == 1, "LVGL object style cache is required");
    static_assert(LV_USE_PPA == 1, "ESP32-P4 PPA drawing is required");
    static_assert(LV_USE_PPA_IMG == 1, "ESP32-P4 PPA image drawing is required");
    static_assert(LV_DEMO_BENCHMARK_ALIGNED_FONTS == 1, "LVGL benchmark aligned fonts are required");

    constexpr char kTag[] = "lvgl_fps_test";
    constexpr int kDisplayWidth = bsp::t_panel_p4_standard::Board::displayWidth;
    constexpr int kDisplayHeight = bsp::t_panel_p4_standard::Board::displayHeight;
    constexpr std::uint32_t kTouchTimeoutMs = 5;
    constexpr std::uint32_t kMinimumLoopDelayMs = 1;
    constexpr std::uint32_t kMaximumLoopDelayMs = 10;

    struct Application
    {
        bsp::t_panel_p4_standard::Board board;
        pdl::touch::JadardTouch *touch{};
        SemaphoreHandle_t refresh_semaphore{};
        lv_area_t pending_refresh_area{};
        bool has_pending_refresh_area{};
    };

    Application application;

    std::uint32_t lvglTickMilliseconds()
    {
        return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
    }

    void flushDisplay(lv_display_t *display, const lv_area_t *area, std::uint8_t *pixel_map)
    {
        auto *app = static_cast<Application *>(
            lv_display_get_user_data(display));
        if (app == nullptr || app->refresh_semaphore == nullptr)
        {
            ESP_LOGE(kTag, "Display context is not initialized");
            lv_display_flush_ready(display);
            return;
        }

        if (app->has_pending_refresh_area)
        {
            app->pending_refresh_area.x1 = std::min(
                app->pending_refresh_area.x1,
                area->x1);
            app->pending_refresh_area.y1 = std::min(
                app->pending_refresh_area.y1,
                area->y1);
            app->pending_refresh_area.x2 = std::max(
                app->pending_refresh_area.x2,
                area->x2);
            app->pending_refresh_area.y2 = std::max(
                app->pending_refresh_area.y2,
                area->y2);
        }
        else
        {
            app->pending_refresh_area = *area;
            app->has_pending_refresh_area = true;
        }

        if (!lv_display_flush_is_last(display))
        {
            lv_display_flush_ready(display);
            return;
        }

        const lv_area_t refresh_area = app->pending_refresh_area;
        app->has_pending_refresh_area = false;
        const esp_err_t error = esp_lcd_panel_draw_bitmap(
            app->board.displayPanel(),
            refresh_area.x1,
            refresh_area.y1,
            refresh_area.x2 + 1,
            refresh_area.y2 + 1,
            pixel_map);
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "Display flush failed: %s", esp_err_to_name(error));
            lv_display_flush_ready(display);
            return;
        }

        // Drop any VSYNC that happened before the framebuffer switch. The next
        // refresh event guarantees that LVGL can safely reuse the other buffer.
        (void)xSemaphoreTake(app->refresh_semaphore, 0);
    }

    void waitForDisplayRefresh(lv_display_t *display)
    {
        auto *app = static_cast<Application *>(
            lv_display_get_user_data(display));
        if (app == nullptr || app->refresh_semaphore == nullptr)
            return;

        (void)xSemaphoreTake(app->refresh_semaphore, portMAX_DELAY);
    }

    bool IRAM_ATTR notifyRefreshDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *user_context)
    {
        auto semaphore = static_cast<SemaphoreHandle_t>(user_context);
        BaseType_t task_woken = pdFALSE;
        xSemaphoreGiveFromISR(semaphore, &task_woken);
        return task_woken == pdTRUE;
    }

    void readTouch(lv_indev_t *input_device, lv_indev_data_t *input_data)
    {
        auto *app = static_cast<Application *>(
            lv_indev_get_user_data(input_device));
        if (app == nullptr || app->touch == nullptr)
        {
            input_data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        pdl::touch::JadardTouchData report{};
        const pdl::Status status =
            app->touch->readTouchData(report, kTouchTimeoutMs);
        if (pdl::isError(status) || report.finger_count == 0U)
        {
            input_data->state = LV_INDEV_STATE_RELEASED;
            return;
        }

        const auto &point = report.points[0];
        input_data->point.x = std::min<std::uint16_t>(
            point.x,
            static_cast<std::uint16_t>(kDisplayWidth - 1));
        input_data->point.y = std::min<std::uint16_t>(
            point.y,
            static_cast<std::uint16_t>(kDisplayHeight - 1));
        input_data->state = LV_INDEV_STATE_PRESSED;
    }

    void logBenchmarkSummary(const lv_demo_benchmark_summary_t *summary)
    {
        if (summary == nullptr)
        {
            ESP_LOGE(kTag, "Benchmark finished without a summary");
            return;
        }

        ESP_LOGI(
            kTag,
            "Benchmark result: scene, CPU, FPS, total, render, flush (ms)");
        for (std::size_t index = 0;
             summary->scenes[index].create_cb != nullptr;
             ++index)
        {
            const auto &scene = summary->scenes[index];
            if (scene.measurement_cnt == 0U)
                continue;

            const std::uint32_t render_time =
                scene.render_avg_time / scene.measurement_cnt;
            const std::uint32_t flush_time =
                scene.flush_avg_time / scene.measurement_cnt;
            ESP_LOGI(
                kTag,
                "%s, %lu%%, %lu FPS, %lu, %lu, %lu",
                scene.name,
                static_cast<unsigned long>(
                    scene.cpu_avg_usage / scene.measurement_cnt),
                static_cast<unsigned long>(
                    scene.fps_avg / scene.measurement_cnt),
                static_cast<unsigned long>(render_time + flush_time),
                static_cast<unsigned long>(render_time),
                static_cast<unsigned long>(flush_time));
        }

        if (summary->valid_scene_cnt > 0)
        {
            const std::int32_t render_time =
                summary->total_avg_render_time / summary->valid_scene_cnt;
            const std::int32_t flush_time =
                summary->total_avg_flush_time / summary->valid_scene_cnt;
            ESP_LOGI(
                kTag,
                "All scenes avg., %ld%%, %ld FPS, %ld, %ld, %ld",
                static_cast<long>(
                    summary->total_avg_cpu / summary->valid_scene_cnt),
                static_cast<long>(
                    summary->total_avg_fps / summary->valid_scene_cnt),
                static_cast<long>(render_time + flush_time),
                static_cast<long>(render_time),
                static_cast<long>(flush_time));
        }
        else
        {
            ESP_LOGW(kTag, "Benchmark produced no valid scene measurements");
        }

        lv_demo_benchmark_summary_display(summary);
    }

    esp_err_t initializeLvgl(Application &app)
    {
        lv_init();
        lv_tick_set_cb(lvglTickMilliseconds);

        lv_display_t *display = lv_display_create(kDisplayWidth, kDisplayHeight);
        ESP_RETURN_ON_FALSE(
            display != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create LVGL display failed");

        app.refresh_semaphore = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(
            app.refresh_semaphore != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create display refresh semaphore failed");

        lv_display_set_user_data(display, &app);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);

        void *first_frame_buffer = app.board.frameBuffer(0);
        void *second_frame_buffer = app.board.frameBuffer(1);
        ESP_RETURN_ON_FALSE(
            app.board.frameBufferCount() >= 2U &&
                first_frame_buffer != nullptr && second_frame_buffer != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "DPI double framebuffers are not available");

        constexpr std::size_t frame_buffer_size = kDisplayWidth * kDisplayHeight * sizeof(std::uint16_t);
        lv_display_set_buffers(
            display,
            first_frame_buffer,
            second_frame_buffer,
            frame_buffer_size,
            LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_set_flush_cb(display, flushDisplay);
        lv_display_set_flush_wait_cb(display, waitForDisplayRefresh);

        esp_lcd_dpi_panel_event_callbacks_t panel_callbacks{};
        panel_callbacks.on_refresh_done = notifyRefreshDone;
        ESP_RETURN_ON_ERROR(
            esp_lcd_dpi_panel_register_event_callbacks(
                app.board.displayPanel(),
                &panel_callbacks,
                app.refresh_semaphore),
            kTag,
            "Register display refresh callback failed");

        lv_indev_t *input_device = lv_indev_create();
        ESP_RETURN_ON_FALSE(
            input_device != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create LVGL touch input failed");
        lv_indev_set_type(input_device, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(input_device, display);
        lv_indev_set_user_data(input_device, &app);
        lv_indev_set_read_cb(input_device, readTouch);

        lv_demo_benchmark_set_end_cb(logBenchmarkSummary);
        lv_demo_benchmark();
        return ESP_OK;
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(application.board.initialize());
    bsp::t_panel_p4_standard::DisplayConfig display_config{};
    display_config.frame_buffer_count = 2;
    ESP_ERROR_CHECK(application.board.initializeDisplay(display_config));
    ESP_ERROR_CHECK(application.board.initializeTouch());
    application.touch = application.board.touchController();
    ESP_ERROR_CHECK(
        application.touch != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(initializeLvgl(application));
    ESP_ERROR_CHECK(application.board.setBacklightBrightness(100));

    ESP_LOGI(
        kTag,
        "LVGL %d.%d.%d benchmark at %dx%d, touch, direct double buffer, PPA, 2 SW draw units",
        LVGL_VERSION_MAJOR,
        LVGL_VERSION_MINOR,
        LVGL_VERSION_PATCH,
        kDisplayWidth,
        kDisplayHeight);

    while (true)
    {
        std::uint32_t delay_ms = lv_timer_handler();
        delay_ms = std::max(delay_ms, kMinimumLoopDelayMs);
        delay_ms = std::min(delay_ms, kMaximumLoopDelayMs);
        TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        if (delay_ticks == 0)
            delay_ticks = 1;
        vTaskDelay(delay_ticks);
    }
}
