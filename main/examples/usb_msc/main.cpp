#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
constexpr char kTag[] = "usb_msc";
bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::Board board;
}

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "starting USB MSC SD-card example");
    ESP_ERROR_CHECK(board.initialize());
    ESP_ERROR_CHECK(board.initializeUsbMsc());

    ESP_LOGI(kTag, "USB MSC ready; connect the USB OTG port to the computer");
    ESP_LOGW(kTag, "SD card is exclusively owned by USB; do not access it locally");

    while (true)
        vTaskDelay(portMAX_DELAY);
}
