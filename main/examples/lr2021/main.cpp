#include <cstdint>
#include <cstdio>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_STANDARD
#include <bsp/t_panel_p4_standard.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE t_panel_p4_standard
#elif T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_ROUND
#include <bsp/t_panel_p4_round.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE t_panel_p4_round
#elif T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_RECT
#error "LR2021 example is not supported on T_PANEL_P4_HARDWARE_VERSION_RECT"
#else
#error "Unsupported T_PANEL_P4_HARDWARE_VERSION"
#endif

namespace
{
    constexpr char kTag[] = "lr2021_example";
    constexpr TickType_t kSendInterval = pdMS_TO_TICKS(5000);

    bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::Board board;

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
    ESP_ERROR_CHECK(board.initialize());

    bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE::LoraConfig config{};
    config.frequency_mhz = 2450.0F;
    config.bandwidth_khz = 125.0F;
    config.spreading_factor = 7;
    config.coding_rate = 5;
    config.output_power_dbm = 5;
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
