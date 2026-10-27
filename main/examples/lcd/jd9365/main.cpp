#include <array>
#include <cstddef>
#include <cstdint>

#include <esp_check.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_standard.hpp>

namespace
{
constexpr char kTag[] = "lcd_jd9365";
constexpr std::array<std::uint16_t, 5> kRgb565Bars{
    0xF800, // Red
    0x07E0, // Green
    0x001F, // Blue
    0xFFFF, // White
    0x0000, // Black
};

bsp::t_panel_p4_standard::Board board;

esp_err_t drawColorBars()
{
    auto *pixels = static_cast<std::uint16_t *>(board.frameBuffer());
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
                static_cast<std::size_t>(x * kRgb565Bars.size() / width);
            pixels[y * width + x] = kRgb565Bars[bar];
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

esp_err_t runExample()
{
    ESP_RETURN_ON_ERROR(board.initialize(), kTag, "BSP initialization failed");
    ESP_RETURN_ON_ERROR(
        board.initializeDisplay(),
        kTag,
        "display initialization failed");
    ESP_RETURN_ON_ERROR(drawColorBars(), kTag, "draw color bars failed");
    ESP_RETURN_ON_ERROR(
        board.setBacklightBrightness(100),
        kTag,
        "enable backlight failed");

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
