#include <array>
#include <cstddef>
#include <cstdint>

#include <bsp/board.hpp>
#include <esp_attr.h>
#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{
    constexpr char kTag[] = "lcd_touch";
    constexpr std::array<std::uint32_t, 5> kColorBars{
        0xFF0000, // Red
        0x00FF00, // Green
        0x0000FF, // Blue
        0xFFFFFF, // White
        0x000000, // Black
    };

    // Owns the panel, its framebuffer and the power/DSI resources for the
    // lifetime of the example. The returned panel handle only borrows them.
    bsp::Board board;

    [[nodiscard]] esp_lcd_panel_handle_t initializeDisplay()
    {
        bsp::DisplayConfig config{};
        config.frame_buffer_count = 1;
        config.in_color_format = LCD_COLOR_FMT_RGB888;
        config.bits_per_pixel = 24;
        ESP_ERROR_CHECK(board.initializeDisplay(config));

        // JD9365, JD9365DA and ILI9882 all expose the same ESP-IDF panel API.
        const esp_lcd_panel_handle_t panel = board.displayPanel();
        ESP_ERROR_CHECK(panel != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);
        return panel;
    }

    esp_err_t drawColorBars(esp_lcd_panel_handle_t panel)
    {
        auto *pixels = static_cast<std::uint8_t *>(board.frameBuffer());
        if (pixels == nullptr)
            return ESP_ERR_INVALID_STATE;

        constexpr int width = bsp::Board::displayWidth;
        constexpr int height = bsp::Board::displayHeight;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::uint32_t color = kColorBars[x * kColorBars.size() / width];
                const std::size_t offset = static_cast<std::size_t>(y * width + x) * 3U;
                pixels[offset] = static_cast<std::uint8_t>(color >> 16U);
                pixels[offset + 1U] = static_cast<std::uint8_t>(color >> 8U);
                pixels[offset + 2U] = static_cast<std::uint8_t>(color);
            }
        }

        return esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, pixels);
    }

    void IRAM_ATTR touchInterruptHandler(void *argument)
    {
        // I2C reads and logging run in the notified task, never in the ISR.
        const TaskHandle_t task = static_cast<TaskHandle_t>(argument);
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE)
            portYIELD_FROM_ISR();
    }

    [[nodiscard]] bsp::TouchController *initializeTouch()
    {
        bsp::TouchConfig config{};
        config.interrupt_handler = touchInterruptHandler;
        config.interrupt_argument = xTaskGetCurrentTaskHandle();
        ESP_ERROR_CHECK(board.initializeTouch(config));

        // The BSP wraps Jadard, GT911 and ILI9882 in the same controller API.
        auto *touch = board.touchController();
        ESP_ERROR_CHECK(touch != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);
        return touch;
    }
}

extern "C" void app_main(void)
{
    const esp_lcd_panel_handle_t display = initializeDisplay();
    ESP_ERROR_CHECK(drawColorBars(display));
    // Keep the backlight off until the first frame has been submitted.
    ESP_ERROR_CHECK(board.setBacklightBrightness(100));
    ESP_LOGI(kTag, "Display ready: %dx%d, RGB888 framebuffer, 24-bit panel",
             bsp::Board::displayWidth, bsp::Board::displayHeight);

    // auto *touch = initializeTouch();
    // ESP_LOGI(kTag, "Touch interrupt ready on GPIO%d",
    //          static_cast<int>(bsp::T_PANEL_P4_BSP_NAMESPACE::pins::touchInterrupt));

    // // Read once in case a report became ready before the callback was installed.
    // xTaskNotifyGive(xTaskGetCurrentTaskHandle());
    // bool touch_pressed = false;
    // while (true)
    // {
    //     if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0U)
    //         continue;

    //     bsp::TouchData report{};
    //     const pdl::Status status = touch->readTouchData(report);
    //     if (status == pdl::Status::busy)
    //         continue;
    //     if (pdl::isError(status))
    //     {
    //         ESP_LOGW(kTag, "Read touch report failed: %s", pdl::statusName(status));
    //         continue;
    //     }

    //     if (report.edge_touch)
    //         ESP_LOGI(kTag, "Edge touch event");
    //     if (report.finger_count == 0U && touch_pressed)
    //         ESP_LOGI(kTag, "Touch released");
    //     touch_pressed = report.finger_count != 0U;

    //     for (std::uint8_t index = 0; index < report.finger_count; ++index)
    //     {
    //         const auto &point = report.points[index];
    //         ESP_LOGI(kTag, "point[%u]: id=%u x=%u y=%u strength=%u",
    //                  static_cast<unsigned int>(index),
    //                  static_cast<unsigned int>(point.track_id),
    //                  static_cast<unsigned int>(point.x),
    //                  static_cast<unsigned int>(point.y),
    //                  static_cast<unsigned int>(point.strength));
    //     }
    // }
}
