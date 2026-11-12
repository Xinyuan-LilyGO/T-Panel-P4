#include <cstdint>
#include <cstdio>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/board.hpp>

#if T_PANEL_P4_BSP_IS_RECT
#error "LR2021 example requires standard or round hardware"
#endif

namespace
{
    constexpr char kTag[] = "lr2021_example";
    constexpr TickType_t kSendInterval = pdMS_TO_TICKS(5000);

    bsp::Board board;

    void runTransmitLoop(LR2021 &radio)
    {
        std::uint32_t sequence{};
        while (true)
        {
            char payload[64]{};
            std::snprintf(
                payload,
                sizeof(payload),
                "T-Panel-P4 LR2021 packet %lu",
                static_cast<unsigned long>(sequence));

            const std::int16_t state = radio.transmit(payload);

            if (state == RADIOLIB_ERR_NONE)
            {
                ESP_LOGI(kTag, "Sent: %s", payload);
                ++sequence;
            }
            else
            {
                ESP_LOGE(
                    kTag,
                    "Transmit failed: RadioLib=%d",
                    static_cast<int>(state));
            }

            vTaskDelay(kSendInterval);
        }
    }
}

extern "C" void app_main(void)
{
    bsp::T_PANEL_P4_BSP_NAMESPACE::LoraConfig config{};
    config.frequency_mhz = 868.0F;
    config.bandwidth_khz = 125.0F;
    config.spreading_factor = 7;
    config.coding_rate = 5;
    config.output_power_dbm = 22;
    config.preamble_length = 12;
    config.crc_enabled = true;

    ESP_ERROR_CHECK(board.initializeLora(config));
    LR2021 *radio = board.loraRadio();
    ESP_ERROR_CHECK(radio != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);

    ESP_LOGI(
        kTag,
        "Starting transmit loop at %.1f MHz, interval=%u ms",
        static_cast<double>(config.frequency_mhz),
        static_cast<unsigned int>(kSendInterval * portTICK_PERIOD_MS));
    runTransmitLoop(*radio);
}
