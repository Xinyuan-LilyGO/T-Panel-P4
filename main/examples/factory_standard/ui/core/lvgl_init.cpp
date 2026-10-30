#include "lvgl_init.hpp"

#include <algorithm>
#include <cstdint>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace factory::ui
{
    namespace
    {
        static_assert(
            LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5 &&
                LVGL_VERSION_PATCH == 0,
            "Factory UI requires LVGL 9.5.0");
        static_assert(
            LV_USE_OS == LV_OS_FREERTOS,
            "Factory UI requires the LVGL FreeRTOS backend");

        constexpr char kTag[] = "factory_lvgl";
        constexpr std::uint32_t kHandlerTaskStackSize = 8192;
        constexpr UBaseType_t kHandlerTaskPriority = 5;
        constexpr std::uint32_t kMinimumHandlerDelayMs = 2;
        constexpr std::uint32_t kMaximumHandlerDelayMs = 20;

        struct LvglContext
        {
            TaskHandle_t handler_task{};
            lv_display_t *display{};
            lv_indev_t *pointer_input{};
        };

        LvglContext context;

        std::uint32_t tickMilliseconds()
        {
            return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
        }

        void handlerTask(void *)
        {
            while (true)
            {
                std::uint32_t delay_ms = lv_timer_handler();
                delay_ms = std::max(delay_ms, kMinimumHandlerDelayMs);
                delay_ms = std::min(delay_ms, kMaximumHandlerDelayMs);
                TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
                if (delay_ticks == 0)
                    delay_ticks = 1;
                vTaskDelay(delay_ticks);
            }
        }
    }

    esp_err_t initializeLvgl(const LvglPort &port)
    {
        ESP_RETURN_ON_FALSE(
            context.display == nullptr && context.handler_task == nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "LVGL is already initialized");
        ESP_RETURN_ON_FALSE(
            port.display.horizontal_resolution > 0 &&
                port.display.vertical_resolution > 0 &&
                port.display.color_format != LV_COLOR_FORMAT_UNKNOWN,
            ESP_ERR_INVALID_ARG,
            kTag,
            "Invalid display geometry or color format");
        ESP_RETURN_ON_FALSE(
            port.display.primary_buffer != nullptr &&
                port.display.buffer_size_bytes > 0U &&
                port.display.flush_callback != nullptr,
            ESP_ERR_INVALID_ARG,
            kTag,
            "Invalid display buffers or flush callback");

        lv_init();
        lv_tick_set_cb(tickMilliseconds);

        context.display = lv_display_create(
            port.display.horizontal_resolution,
            port.display.vertical_resolution);
        ESP_RETURN_ON_FALSE(
            context.display != nullptr,
            ESP_ERR_NO_MEM,
            kTag,
            "Create LVGL display failed");

        lv_display_set_user_data(context.display, port.display.user_data);
        lv_display_set_color_format(context.display, port.display.color_format);
        lv_display_set_buffers(
            context.display,
            port.display.primary_buffer,
            port.display.secondary_buffer,
            port.display.buffer_size_bytes,
            port.display.render_mode);
        lv_display_set_flush_cb(
            context.display,
            port.display.flush_callback);
        if (port.display.flush_wait_callback != nullptr)
        {
            lv_display_set_flush_wait_cb(
                context.display,
                port.display.flush_wait_callback);
        }

        if (port.pointer_input.read_callback != nullptr)
        {
            context.pointer_input = lv_indev_create();
            if (context.pointer_input == nullptr)
            {
                lv_display_delete(context.display);
                context.display = nullptr;
                return ESP_ERR_NO_MEM;
            }
            lv_indev_set_type(context.pointer_input, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(context.pointer_input, context.display);
            lv_indev_set_user_data(
                context.pointer_input,
                port.pointer_input.user_data);
            lv_indev_set_read_cb(
                context.pointer_input,
                port.pointer_input.read_callback);
            // Require a deliberate horizontal swipe before page navigation. This
            // keeps slider drags and short touch movements inside controls.
            lv_indev_set_gesture_min_distance(context.pointer_input, 160);
        }

        if (port.create_handler_task)
        {
            const BaseType_t task_created = xTaskCreate(
                handlerTask,
                "factory_lvgl",
                kHandlerTaskStackSize,
                nullptr,
                kHandlerTaskPriority,
                &context.handler_task);
            if (task_created != pdPASS)
            {
                if (context.pointer_input != nullptr)
                {
                    lv_indev_delete(context.pointer_input);
                    context.pointer_input = nullptr;
                }
                lv_display_delete(context.display);
                context.display = nullptr;
                return ESP_ERR_NO_MEM;
            }
        }

        ESP_LOGI(
            kTag,
            "LVGL %d.%d.%d initialized: %ldx%ld, color format=%d, handler=%s",
            LVGL_VERSION_MAJOR,
            LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH,
            static_cast<long>(port.display.horizontal_resolution),
            static_cast<long>(port.display.vertical_resolution),
            static_cast<int>(port.display.color_format),
            port.create_handler_task ? "internal" : "external");
        return ESP_OK;
    }

    void pauseLvglInputState()
    {
        if (context.pointer_input == nullptr)
            return;
        lv_indev_enable(context.pointer_input, false);
        lv_timer_t *read_timer = lv_indev_get_read_timer(context.pointer_input);
        if (read_timer != nullptr)
            lv_timer_pause(read_timer);
    }

    void resetLvglInputState()
    {
        if (context.pointer_input == nullptr)
            return;
        // Keep the application-specific user data installed by the port.
        // This module's private context has a different type and must not
        // replace the callback context used by the input driver.
        lv_indev_set_mode(context.pointer_input, LV_INDEV_MODE_TIMER);
        lv_indev_reset(context.pointer_input, nullptr);
        lv_indev_enable(context.pointer_input, true);
    lv_timer_t *read_timer = lv_indev_get_read_timer(context.pointer_input);
    if (read_timer != nullptr)
    {
        lv_timer_resume(read_timer);
        ESP_LOGI(kTag, "LVGL input read timer resumed (paused=%d)",
                 lv_timer_get_paused(read_timer) ? 1 : 0);
    }
    }
}
