#include <array>
#include <cstdint>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <pdl/core/status.hpp>
#include <pdl/drivers/touch/jadard/jadard.hpp>

namespace
{
constexpr char kTag[] = "jadard_example";
bsp::t_panel_p4_standard::Board board;

void IRAM_ATTR touchInterruptHandler(void *argument)
{
    const TaskHandle_t task = static_cast<TaskHandle_t>(argument);
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE)
        portYIELD_FROM_ISR();
}

void logControllerInformation(pdl::touch::JadardTouch &touch)
{
    std::array<std::uint8_t, 2> chip_id{};
    pdl::Status status = touch.readChipId(chip_id);
    if (pdl::isOk(status))
    {
        ESP_LOGI(kTag, "Chip ID: %02X%02X", chip_id[0], chip_id[1]);
    }
    else
    {
        ESP_LOGW(kTag, "Read chip ID failed: %s", pdl::statusName(status));
    }

    std::array<std::uint8_t, 8> firmware{};
    status = touch.readFirmwareVersion(firmware);
    if (pdl::isOk(status))
    {
        ESP_LOGI(
            kTag,
            "Firmware AP=%u.%u.%u.%u MP=%u.%u.%u.%u",
            firmware[0],
            firmware[1],
            firmware[2],
            firmware[3],
            firmware[4],
            firmware[5],
            firmware[6],
            firmware[7]);
    }
    else
    {
        ESP_LOGW(kTag, "Read firmware failed: %s", pdl::statusName(status));
    }

    std::uint16_t x_resolution{};
    std::uint16_t y_resolution{};
    status = touch.readResolution(x_resolution, y_resolution);
    if (pdl::isOk(status))
    {
        ESP_LOGI(kTag, "Resolution: %ux%u", x_resolution, y_resolution);
    }
    else
    {
        ESP_LOGW(kTag, "Read resolution failed: %s", pdl::statusName(status));
    }
}
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(board.initialize());
    ESP_ERROR_CHECK(board.initializeTouch());

    auto *touch = board.touchController();
    ESP_ERROR_CHECK(touch != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);
    logControllerInformation(*touch);

    ESP_ERROR_CHECK(board.initializeTouchInterrupt(
        touchInterruptHandler,
        xTaskGetCurrentTaskHandle(),
        GPIO_INTR_NEGEDGE));

    ESP_LOGI(kTag, "Touch interrupt ready on GPIO%d", 
             bsp::t_panel_p4_standard::pins::touchInterrupt);

    while (true)
    {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0U)
            continue;

        pdl::touch::JadardTouchData data{};
        const pdl::Status status = touch->readTouchData(data);
        if (pdl::isError(status))
        {
            ESP_LOGW(kTag, "Read touch report failed: %s", pdl::statusName(status));
            continue;
        }

        if (data.edge_touch)
            ESP_LOGI(kTag, "Edge touch event");
        for (std::uint8_t index = 0; index < data.finger_count; ++index)
        {
            const auto &point = data.points[index];
            ESP_LOGI(
                kTag,
                "point[%u]: x=%u y=%u strength=%u",
                index,
                point.x,
                point.y,
                point.strength);
        }
    }
}
