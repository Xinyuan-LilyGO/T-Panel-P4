#include <array>
#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_standard.hpp>

namespace
{
    constexpr char kTag[] = "lcd_jd9365";
    constexpr gpio_num_t kSleepButton = bsp::t_panel_p4_standard::pins::button;
    constexpr int kBacklightBrightness = 50;
    constexpr TickType_t kButtonPollPeriod = pdMS_TO_TICKS(20);
    constexpr std::array<std::uint32_t, 5> kRgbBars{
        0xFF0000, // Red
        0x00FF00, // Green
        0x0000FF, // Blue
        0xFFFFFF, // White
        0x000000, // Black
    };

    bsp::t_panel_p4_standard::Board board;

    esp_err_t drawColorBars()
    {
        auto *pixels = static_cast<std::uint8_t *>(board.frameBuffer());
        ESP_RETURN_ON_FALSE(
            pixels != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "display framebuffer is unavailable");

        constexpr int width = bsp::t_panel_p4_standard::Board::displayWidth;
        constexpr int height = bsp::t_panel_p4_standard::Board::displayHeight;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::size_t bar =
                    static_cast<std::size_t>(x * kRgbBars.size() / width);
                const std::uint32_t color = kRgbBars[bar];
                const std::size_t pixel =
                    static_cast<std::size_t>(y * width + x);

                if constexpr (bsp::t_panel_p4_standard::Board::displayBitsPerPixel == 16)
                {
                    const std::uint16_t rgb565 = static_cast<std::uint16_t>(
                        (((color >> 19U) & 0x1FU) << 11U) |
                        (((color >> 10U) & 0x3FU) << 5U) |
                        ((color >> 3U) & 0x1FU));
                    auto *pixels565 = reinterpret_cast<std::uint16_t *>(pixels);
                    pixels565[pixel] = rgb565;
                }
                else if constexpr (bsp::t_panel_p4_standard::Board::displayBitsPerPixel == 24)
                {
                    const std::size_t offset = pixel * 3U;
                    pixels[offset] = static_cast<std::uint8_t>(color >> 16U);
                    pixels[offset + 1U] = static_cast<std::uint8_t>(color >> 8U);
                    pixels[offset + 2U] = static_cast<std::uint8_t>(color);
                }
                else
                {
                    static_assert(
                        bsp::t_panel_p4_standard::Board::displayBitsPerPixel == 16 ||
                            bsp::t_panel_p4_standard::Board::displayBitsPerPixel == 24,
                        "Unsupported JD9365 framebuffer format");
                }
            }
        }

        return esp_lcd_panel_draw_bitmap(
            board.displayPanel(),
            0,
            0,
            width,
            height,
            pixels);
    }

    void buttonTask(void *)
    {
        bool previous_pressed = false;

        while (true)
        {
            const bool pressed = gpio_get_level(kSleepButton) == 0;
            if (pressed && !previous_pressed)
            {
                esp_err_t error = board.setDisplaySleep(true);
                if (error != ESP_OK)
                {
                    ESP_LOGE(kTag, "LCD sleep failed: %s", esp_err_to_name(error));
                }
                else
                {
                    ESP_LOGI(kTag, "LCD sleeping; release GPIO%d to enter light sleep",
                             static_cast<int>(kSleepButton));
                    do
                    {
                        vTaskDelay(kButtonPollPeriod);
                    } while (gpio_get_level(kSleepButton) == 0);

                    bsp::t_panel_p4_standard::LightSleepConfig sleep_config{};
                    sleep_config.wakeup_gpio = kSleepButton;
                    sleep_config.wakeup_on_high = false;
                    error = board.enterLightSleep(sleep_config);
                    if (error != ESP_OK)
                    {
                        ESP_LOGE(kTag, "P4 light sleep failed: %s",
                                 esp_err_to_name(error));
                    }                 
            
                    drawColorBars();

                    previous_pressed = true;
                    continue;
                }
            }
            previous_pressed = pressed;
            vTaskDelay(kButtonPollPeriod);
        }
    }

    esp_err_t runExample()
    {
        ESP_RETURN_ON_ERROR(board.initialize(), kTag, "BSP initialization failed");
        ESP_RETURN_ON_ERROR(
            board.initializeDisplay(),
            kTag,
            "display initialization failed");
        ESP_RETURN_ON_ERROR(drawColorBars(), kTag, "draw color bars failed");
        ESP_RETURN_ON_ERROR(
            board.setBacklightBrightness(kBacklightBrightness),
            kTag,
            "enable backlight failed");

        gpio_config_t button_config{};
        button_config.pin_bit_mask = std::uint64_t{1} << kSleepButton;
        button_config.mode = GPIO_MODE_INPUT;
        button_config.pull_up_en = GPIO_PULLUP_ENABLE;
        button_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        button_config.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(
            gpio_config(&button_config),
            kTag,
            "configure GPIO%d sleep button failed",
            static_cast<int>(kSleepButton));

        if (xTaskCreate(buttonTask, "jd9365_button", 3072, nullptr, 5, nullptr) !=
            pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(kTag, "JD9365 color bars ready through board BSP");
        return ESP_OK;
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(runExample());
    while (true)
        vTaskDelay(portMAX_DELAY);
}
