#include "lvgl_port.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

#include <bsp/board.hpp>
#include <pdl/core/status.hpp>
#include <pdl/drivers/touch/gt911/gt911.hpp>

#include "lvgl_init.hpp"

namespace factory::ui
{
    namespace
    {
        constexpr char kTag[] = "factory_round_lvgl";
        constexpr std::uint32_t kTouchTimeoutMs = 5;

        struct RoundLvglContext
        {
            bsp::t_panel_p4_round::Board *board{};
            bsp::TouchController *touch{};
            SemaphoreHandle_t refresh_semaphore{};
            SemaphoreHandle_t te_semaphore{};
            bool te_enabled{};
            bool refresh_pending{};
            bool display_powered_down{};
            lv_point_t last_point{};
            lv_indev_state_t last_input_state{LV_INDEV_STATE_RELEASED};
        };

        RoundLvglContext context;
        FramebufferOverlay s_framebuffer_overlay{};
        std::uint32_t s_flush_count{};
        std::int64_t s_flush_window_us{};

        void flushDisplay(
            lv_display_t *display,
            const lv_area_t *,
            std::uint8_t *pixel_map)
        {
            const std::int64_t flush_start_us = esp_timer_get_time();
            auto *port = static_cast<RoundLvglContext *>(
                lv_display_get_user_data(display));
            if (port == nullptr || port->board == nullptr ||
                port->refresh_semaphore == nullptr)
            {
                ESP_LOGE(kTag, "Display port is not initialized");
                lv_display_flush_ready(display);
                return;
            }

            if (port->display_powered_down)
            {
                lv_display_flush_ready(display);
                return;
            }

            // FULL mode supplies a complete screen-sized framebuffer. Submit
            // it once after LVGL has rendered the frame.
            if (!lv_display_flush_is_last(display))
            {
                lv_display_flush_ready(display);
                return;
            }
#if CONFIG_FACTORY_ROUND_LCD_USE_TE
            if (port->te_enabled && port->te_semaphore != nullptr)
            {
                // Discard an already latched pulse and wait for the next TE
                // edge before changing the DPI driver's active framebuffer.
                while (xSemaphoreTake(port->te_semaphore, 0) == pdTRUE)
                    ;
                (void)xSemaphoreTake(
                    port->te_semaphore,
                    pdMS_TO_TICKS(20));
            }
#endif
            if (s_framebuffer_overlay != nullptr)
                s_framebuffer_overlay(pixel_map);
            const esp_err_t error = esp_lcd_panel_draw_bitmap(
                port->board->displayPanel(),
                0,
                0,
                bsp::t_panel_p4_round::Board::displayWidth,
                bsp::t_panel_p4_round::Board::displayHeight,
                pixel_map);
            const std::int64_t submit_us = esp_timer_get_time() - flush_start_us;
            if (error != ESP_OK)
            {
                ESP_LOGE(kTag, "Display flush failed: %s", esp_err_to_name(error));
                lv_display_flush_ready(display);
            }
            else
            {
                // Discard VSYNC notifications that may have happened before
                // or during the framebuffer switch. Waiting for the next one
                // ensures LVGL won't reuse the other buffer too early.
                (void)xSemaphoreTake(port->refresh_semaphore, 0);
                port->refresh_pending = true;
                ++s_flush_count;
                if (s_flush_window_us == 0)
                    s_flush_window_us = esp_timer_get_time();
                if (esp_timer_get_time() - s_flush_window_us >= 1'000'000)
                {
                    ESP_LOGI(kTag, "LVGL flush: %u FPS, submit=%lld us",
                             static_cast<unsigned>(s_flush_count),
                             static_cast<long long>(submit_us));
                    s_flush_count = 0;
                    s_flush_window_us = esp_timer_get_time();
                }
            }
        }

        void waitForDisplayRefresh(lv_display_t *display)
        {
            auto *port = static_cast<RoundLvglContext *>(
                lv_display_get_user_data(display));
            if (port == nullptr || port->refresh_semaphore == nullptr)
                return;

            if (!port->refresh_pending)
                return;

            const std::int64_t wait_start_us = esp_timer_get_time();
            (void)xSemaphoreTake(port->refresh_semaphore, portMAX_DELAY);
            const std::int64_t wait_us = esp_timer_get_time() - wait_start_us;
            if (wait_us > 20'000)
                ESP_LOGI(kTag, "LVGL display wait: %lld us",
                         static_cast<long long>(wait_us));
            port->refresh_pending = false;
        }

        bool IRAM_ATTR notifyRefreshDone(
            esp_lcd_panel_handle_t,
            esp_lcd_dpi_panel_event_data_t *,
            void *user_context)
        {
            auto semaphore = static_cast<SemaphoreHandle_t>(user_context);
            BaseType_t task_woken = pdFALSE;
            xSemaphoreGiveFromISR(semaphore, &task_woken);
            return task_woken == pdTRUE;
        }

#if CONFIG_FACTORY_ROUND_LCD_USE_TE
        void IRAM_ATTR notifyTearingEffect(void *user_context)
        {
            auto semaphore = static_cast<SemaphoreHandle_t>(user_context);
            BaseType_t task_woken = pdFALSE;
            xSemaphoreGiveFromISR(semaphore, &task_woken);
            if (task_woken == pdTRUE)
                portYIELD_FROM_ISR();
        }
#endif

        void readTouch(lv_indev_t *input_device, lv_indev_data_t *input_data)
        {
            auto *port = static_cast<RoundLvglContext *>(
                lv_indev_get_user_data(input_device));
            // The BSP reconstructs the optional GT911 object after light
            // sleep. Refresh this pointer on every read so LVGL can never
            // retain a pointer to the pre-sleep object storage.
            auto *touch = (port != nullptr && port->board != nullptr)
                              ? port->board->touchController()
                              : nullptr;
            if (port == nullptr || touch == nullptr)
            {
                input_data->state = LV_INDEV_STATE_RELEASED;
                return;
            }

            bsp::TouchData report{};
            const pdl::Status status =
                touch->readTouchData(report, kTouchTimeoutMs);
            if (status == pdl::Status::busy)
            {
                // GT911 only reports a new frame when its status register is
                // ready. Preserve the previous LVGL state between frames so
                // a slow poll does not create an artificial release.
                input_data->point = port->last_point;
                input_data->state = port->last_input_state;
                return;
            }
            if (pdl::isError(status))
            {
                static TickType_t last_error_log{};
                const TickType_t now = xTaskGetTickCount();
                if (now - last_error_log >= pdMS_TO_TICKS(1000))
                {
                    ESP_LOGW(kTag, "Touch read failed: status=%d",
                             static_cast<int>(status));
                    last_error_log = now;
                }
                input_data->state = LV_INDEV_STATE_RELEASED;
                port->last_input_state = LV_INDEV_STATE_RELEASED;
                return;
            }

            if (report.finger_count == 0U)
            {
                input_data->state = LV_INDEV_STATE_RELEASED;
                port->last_input_state = LV_INDEV_STATE_RELEASED;
                return;
            }

            const auto &point = report.points[0];
            input_data->point.x = std::min<std::uint16_t>(
                point.x,
                static_cast<std::uint16_t>(
                    bsp::t_panel_p4_round::Board::displayWidth - 1));
            input_data->point.y = std::min<std::uint16_t>(
                point.y,
                static_cast<std::uint16_t>(
                    bsp::t_panel_p4_round::Board::displayHeight - 1));
            input_data->state = LV_INDEV_STATE_PRESSED;
            port->last_point = input_data->point;
            port->last_input_state = LV_INDEV_STATE_PRESSED;
        }
    }

    void setFramebufferOverlay(FramebufferOverlay overlay)
    {
        s_framebuffer_overlay = overlay;
    }

    void prepareLvglPowerDown(bool stop_display)
    {
        lv_lock();
        // The touch controller is destroyed immediately after this function
        // returns. Stop LVGL's input timer while holding the LVGL lock so no
        // readTouch callback can race with I2C device removal.
        pauseLvglInputState();
        context.touch = nullptr;
        if (stop_display)
        {
            // Finish pending DMA before the BSP frees the framebuffers.
            lv_refr_now(nullptr);
            lv_display_t *display = lv_display_get_default();
            if (display != nullptr)
            {
                if (context.refresh_pending)
                {
                    waitForDisplayRefresh(display);
                    lv_display_flush_ready(display);
                }
                lv_timer_pause(lv_display_get_refr_timer(display));
            }
            context.display_powered_down = true;
        }
        lv_unlock();
    }

    void resumeLvglDisplay()
    {
        ESP_LOGI(kTag, "LVGL resume: enter");
        lv_lock();
        ESP_LOGI(kTag, "LVGL resume: lock acquired");
        if (context.board != nullptr)
        {
            context.touch = context.board->touchController();
        }
        context.last_input_state = LV_INDEV_STATE_RELEASED;
        lv_display_t *display = lv_display_get_default();
        ESP_LOGI(kTag, "LVGL resume: display=%p board_display=%d",
                 static_cast<void *>(display),
                 context.board != nullptr && context.board->isDisplayInitialized());
        if (display != nullptr && context.board != nullptr &&
            context.board->isDisplayInitialized())
        {
            void *primary_buffer = context.board->frameBuffer(0);
            void *secondary_buffer = context.board->frameBuffer(1);
            ESP_LOGI(kTag, "LVGL resume: buffers primary=%p secondary=%p",
                     primary_buffer, secondary_buffer);
            if (primary_buffer == nullptr || secondary_buffer == nullptr)
            {
                ESP_LOGE(kTag, "LVGL display buffers unavailable after wake");
                lv_unlock();
                return;
            }

            constexpr std::size_t frame_buffer_size =
                bsp::t_panel_p4_round::Board::displayWidth *
                bsp::t_panel_p4_round::Board::displayHeight *
                (bsp::t_panel_p4_round::Board::displayBitsPerPixel / 8U);
            // Board light-sleep recovery recreates the DPI framebuffers. LVGL
            // must be pointed at the new allocations before the first flush.
            lv_display_set_user_data(display, &context);
            lv_display_set_buffers(
                display,
                primary_buffer,
                secondary_buffer,
                frame_buffer_size,
                LV_DISPLAY_RENDER_MODE_DIRECT);
            ESP_LOGI(kTag, "LVGL resume: buffers rebound");
            // deinitializeDisplay() deleted the old DPI panel, so its
            // refresh callback was deleted with it. Re-register the callback
            // on the newly created panel before forcing the first flush;
            // otherwise LVGL waits forever for a refresh semaphore that can
            // no longer be signalled.
            esp_lcd_dpi_panel_event_callbacks_t panel_callbacks{};
            panel_callbacks.on_refresh_done = notifyRefreshDone;
            const esp_err_t callback_error =
                esp_lcd_dpi_panel_register_event_callbacks(
                    context.board->displayPanel(),
                    &panel_callbacks,
                    context.refresh_semaphore);
            ESP_LOGI(kTag, "LVGL resume: refresh callback result=%s",
                     esp_err_to_name(callback_error));
            if (callback_error != ESP_OK)
            {
                ESP_LOGE(kTag, "Register display refresh callback after wake failed: %s",
                         esp_err_to_name(callback_error));
                lv_unlock();
                return;
            }
            resetLvglInputState();
            ESP_LOGI(kTag, "LVGL resume: input state reset");
            context.display_powered_down = false;
            lv_timer_resume(lv_display_get_refr_timer(display));
            ESP_LOGI(kTag, "LVGL resume: display refresh timer resumed");

            // DPI resume deliberately starts from framebuffer zero.  LVGL
            // may otherwise have no dirty area after the timer pause and the
            // panel would keep scanning the reset framebuffer contents.
            // Invalidate and submit one complete frame before returning to
            // normal timer-driven refreshes.
            // 使当前活动屏幕的整个区域失效，触发重绘
            lv_obj_invalidate(lv_screen_active());
            ESP_LOGI(kTag, "LVGL display binding restored after wake");
        }
        lv_unlock();
        ESP_LOGI(kTag, "LVGL resume: exit");
    }

    esp_err_t initializeLvgl(bsp::t_panel_p4_round::Board &board)
    {
        ESP_RETURN_ON_FALSE(
            board.isDisplayInitialized(),
            ESP_ERR_INVALID_STATE,
            kTag,
            "BSP display is not initialized");
        // 没有带触摸的屏幕 先注释掉
        // ESP_RETURN_ON_FALSE(
        //     board.isTouchInitialized(),
        //     ESP_ERR_INVALID_STATE,
        //     kTag,
        //     "BSP touch is not initialized");
        ESP_RETURN_ON_FALSE(
            board.frameBufferCount() >= 2U && board.frameBuffer(0) != nullptr &&
                board.frameBuffer(1) != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "DPI double framebuffers are not available");
            
        // 没有带触摸的屏幕 先注释掉
        // bsp::TouchController *touch = board.touchController();
        // ESP_RETURN_ON_FALSE(
        //     touch != nullptr,
        //     ESP_ERR_INVALID_STATE,
        //     kTag,
        //     "GT911 touch controller is not available");

        context.board = &board;
        // context.touch = touch;
        context.display_powered_down = false;
        context.refresh_semaphore = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(
            context.refresh_semaphore != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create display refresh semaphore failed");

#if CONFIG_FACTORY_ROUND_LCD_USE_TE
        context.te_semaphore = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(
            context.te_semaphore != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create LCD TE semaphore failed");

        gpio_config_t te_config{};
        te_config.pin_bit_mask =
            std::uint64_t{1} << bsp::t_panel_p4_round::pins::lcdTearingEffect;
        te_config.mode = GPIO_MODE_INPUT;
        te_config.pull_up_en = GPIO_PULLUP_DISABLE;
        te_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        te_config.intr_type = GPIO_INTR_POSEDGE;
        ESP_RETURN_ON_ERROR(
            gpio_config(&te_config),
            kTag,
            "Configure LCD TE GPIO failed");

        esp_err_t te_isr_error = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        if (te_isr_error != ESP_OK && te_isr_error != ESP_ERR_INVALID_STATE)
        {
            ESP_RETURN_ON_ERROR(
                te_isr_error,
                kTag,
                "Install LCD TE ISR service failed");
        }
        ESP_RETURN_ON_ERROR(
            gpio_isr_handler_add(
                bsp::t_panel_p4_round::pins::lcdTearingEffect,
                notifyTearingEffect,
                context.te_semaphore),
            kTag,
            "Register LCD TE ISR failed");
        context.te_enabled = true;
#endif

        esp_lcd_dpi_panel_event_callbacks_t panel_callbacks{};
        panel_callbacks.on_refresh_done = notifyRefreshDone;
        ESP_RETURN_ON_ERROR(
            esp_lcd_dpi_panel_register_event_callbacks(
                board.displayPanel(),
                &panel_callbacks,
                context.refresh_semaphore),
            kTag,
            "Register display refresh callback failed");

        static_assert(
            bsp::t_panel_p4_round::Board::displayBitsPerPixel == 24,
            "Round LVGL port requires a 24-bit RGB888 framebuffer");
        constexpr lv_color_format_t color_format = LV_COLOR_FORMAT_RGB888;
        constexpr std::size_t frame_buffer_size =
            bsp::t_panel_p4_round::Board::displayWidth *
            bsp::t_panel_p4_round::Board::displayHeight *
            (bsp::t_panel_p4_round::Board::displayBitsPerPixel / 8U);

        LvglPort port{};
        port.display.horizontal_resolution =
            bsp::t_panel_p4_round::Board::displayWidth;
        port.display.vertical_resolution =
            bsp::t_panel_p4_round::Board::displayHeight;
        port.display.color_format = color_format;
        port.display.primary_buffer = board.frameBuffer(0);
        port.display.secondary_buffer = board.frameBuffer(1);
        port.display.buffer_size_bytes = frame_buffer_size;
        // DPI resume always restarts from framebuffer zero.  Full-frame mode
        // makes every flush self-contained and keeps both scanout buffers
        // identical, so a stale second buffer cannot produce a shifted or
        // partially recolored frame after wake.
        port.display.render_mode = LV_DISPLAY_RENDER_MODE_DIRECT;
        port.display.flush_callback = flushDisplay;
        port.display.flush_wait_callback = waitForDisplayRefresh;
        port.display.user_data = &context;
        port.pointer_input.read_callback = readTouch;
        port.pointer_input.user_data = &context;
        // lvgl_page_manager owns the LVGL timer loop and serializes page
        // changes, matching the factory_standard configuration.
        port.create_handler_task = false;
        return initializeLvgl(port);
    }
}
