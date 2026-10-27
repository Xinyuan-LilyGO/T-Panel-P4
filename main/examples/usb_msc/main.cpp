#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_standard.hpp>

namespace
{
constexpr char kTag[] = "usb_msc";
bsp::t_panel_p4_standard::Board board;
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
