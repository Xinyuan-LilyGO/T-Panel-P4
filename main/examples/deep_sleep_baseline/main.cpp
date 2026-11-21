#include <bsp/board.hpp>
#include <esp_log.h>
#include <esp_sleep.h>

static constexpr auto kTag = "deep_sleep_example";

extern "C" void app_main()
{
    const auto cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(kTag, "Wakeup cause: %d", static_cast<int>(cause));

    bsp::Board board;
    ESP_ERROR_CHECK(board.initializePower());
#if T_PANEL_P4_BSP_IS_STANDARD || T_PANEL_P4_BSP_IS_ROUND
    board.ioExpander();
#elif T_PANEL_P4_BSP_IS_RECT
    ESP_LOGI(kTag, "BSP AXP517 initialized; Rect power controls use P4 GPIOs");
#endif

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(kTag, "Entering deep sleep through BSP without a wakeup source; reset or power cycle to restart");
    ESP_ERROR_CHECK(board.enterDeepSleep());
}
