#include <array>
#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_rect.hpp>
#include <pdl/drivers/display/ili9882/ili9882.hpp>
#include <pdl/drivers/touch/ili9882/ili9882.hpp>

namespace
{
    constexpr char kTag[] = "lcd_ili9882";
    constexpr std::array<std::uint16_t, 5> kRgb565Bars{
        0xF800,
        0x07E0,
        0x001F,
        0xFFFF,
        0x0000,
    };

    bsp::t_panel_p4_rect::Board board;

    esp_err_t drawColorBars()
    {
        auto *pixels = static_cast<std::uint16_t *>(board.frameBuffer());
        ESP_RETURN_ON_FALSE(
            pixels != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "display framebuffer is unavailable");

        constexpr int width = bsp::t_panel_p4_rect::Board::displayWidth;
        constexpr int height = bsp::t_panel_p4_rect::Board::displayHeight;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::size_t bar =
                    static_cast<std::size_t>(x * kRgb565Bars.size() / width);
                pixels[y * width + x] = kRgb565Bars[bar];
            }
        }

        return esp_lcd_panel_draw_bitmap(
            board.displayPanel(), 0, 0, width, height, pixels);
    }

    esp_err_t enterPanelAndTouchSleep()
    {
        auto *touch = board.touchController();
        ESP_RETURN_ON_FALSE(
            touch != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "ILI9882 touch controller is unavailable");

        const pdl::Status touch_status = touch->setState(
            pdl::touch::Ili9882State::low_power);
        ESP_RETURN_ON_FALSE(
            pdl::isOk(touch_status),
            ESP_FAIL,
            kTag,
            "enter ILI9882 touch sleep failed: %s",
            pdl::statusName(touch_status));
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_RETURN_ON_ERROR(
            pdl::display::setPanelState(
                board.displayPanel(),
                pdl::display::Ili9882State::sleeping),
            kTag,
            "enter ILI9882 display sleep failed");
        ESP_RETURN_ON_ERROR(
            board.setBacklightBrightness(0),
            kTag,
            "turn off backlight failed");
        vTaskDelay(pdMS_TO_TICKS(3000));

        board.deinitializeDisplay();
        board.setSpeakerEnabled(false);
        board.setDisplayPowerEnabled(false);

        // vTaskDelay(pdMS_TO_TICKS(3000));
        // ESP_LOGI(kTag, "ILI9882 display and touch entered sleep mode");
        // bsp::t_panel_p4_rect::DeepSleepConfig deep_sleep_config{};
        // deep_sleep_config.wakeup_gpio = bsp::t_panel_p4_rect::pins::button;
        // deep_sleep_config.wakeup_on_high = false;
        // ESP_RETURN_ON_ERROR(
        //     board.enterDeepSleep(deep_sleep_config),
        //     kTag,
        //     "enter ESP32-P4 deep sleep failed");
        return ESP_OK;
    }

    esp_err_t runExample()
    {
        ESP_RETURN_ON_ERROR(board.initialize(), kTag, "RECT BSP initialization failed");
        ESP_RETURN_ON_ERROR(
            board.initializeDisplay(),
            kTag,
            "ILI9882 display initialization failed");
        bsp::t_panel_p4_rect::TouchConfig touch_config{};
        touch_config.perform_software_reset = false;
        ESP_RETURN_ON_ERROR(
            board.initializeTouch(touch_config),
            kTag,
            "ILI9882 touch initialization failed");
        ESP_RETURN_ON_ERROR(drawColorBars(), kTag, "draw color bars failed");
        ESP_RETURN_ON_ERROR(
            board.setBacklightBrightness(100),
            kTag,
            "enable backlight failed");

        ESP_LOGI(kTag, "ILI9882 720x1440 color bars ready through RECT BSP");
        vTaskDelay(pdMS_TO_TICKS(5000));
        return enterPanelAndTouchSleep();
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(runExample());
    while (true)
        vTaskDelay(portMAX_DELAY);
}
