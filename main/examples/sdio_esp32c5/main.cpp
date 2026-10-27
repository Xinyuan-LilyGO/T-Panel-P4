#include <cstdint>
#include <cstdlib>
#include <ctime>

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <bsp/t_panel_p4_standard_pins.hpp>
#include <esp32c5_connectivity/esp32c5_connectivity.hpp>

#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

namespace
{
    constexpr char kTag[] = "sdio_esp32c5";
    constexpr char kSntpServer[] = "pool.ntp.org";
    constexpr char kTimeZone[] = "CST-8";
    constexpr std::uint32_t kWifiConnectionTimeoutMs = 30000;
    constexpr std::uint8_t kSntpSyncAttempts = 10;

    esp_err_t synchronizeAndPrintTime()
    {
        esp_sntp_config_t sntp_config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG(kSntpServer);
        esp_err_t result = esp_netif_sntp_init(&sntp_config);
        if (result != ESP_OK)
            return result;

        result = ESP_ERR_TIMEOUT;
        for (std::uint8_t attempt = 0; attempt < kSntpSyncAttempts; ++attempt)
        {
            result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
            if (result == ESP_OK)
                break;
            if (result != ESP_ERR_TIMEOUT && result != ESP_ERR_NOT_FINISHED)
                break;
            ESP_LOGI(
                kTag,
                "waiting for SNTP time synchronization (%u/%u)",
                static_cast<unsigned>(attempt + 1U),
                static_cast<unsigned>(kSntpSyncAttempts));
        }

        if (result == ESP_OK)
        {
            if (setenv("TZ", kTimeZone, 1) != 0)
                result = ESP_FAIL;
            else
                tzset();
        }

        if (result == ESP_OK)
        {
            std::time_t now{};
            std::tm local_time{};
            char time_text[64]{};
            (void)std::time(&now);
            if (localtime_r(&now, &local_time) == nullptr ||
                std::strftime(
                    time_text,
                    sizeof(time_text),
                    "%Y-%m-%d %H:%M:%S %Z",
                    &local_time) == 0U)
            {
                result = ESP_FAIL;
            }
            else
            {
                ESP_LOGI(kTag, "current time: %s", time_text);
            }
        }

        esp_netif_sntp_deinit();
        return result;
    }
}

extern "C" void app_main()
{
    bsp::t_panel_p4_standard::Board board;
    ESP_ERROR_CHECK(board.initialize());

    connectivity::Esp32c5ConnectivityConfig config{};
    config.expected_sdio.clock_gpio =
        bsp::t_panel_p4_standard::pins::sd2Clock;
    config.expected_sdio.command_gpio =
        bsp::t_panel_p4_standard::pins::sd2Command;
    config.expected_sdio.data0_gpio =
        bsp::t_panel_p4_standard::pins::sd2Data0;
    config.expected_sdio.data1_gpio =
        bsp::t_panel_p4_standard::pins::sd2Data1;
    config.expected_sdio.data2_gpio =
        bsp::t_panel_p4_standard::pins::sd2Data2;
    config.expected_sdio.data3_gpio =
        bsp::t_panel_p4_standard::pins::sd2Data3;
    config.expected_sdio.enable_gpio =
        bsp::t_panel_p4_standard::pins::esp32c5Enable;

    connectivity::Esp32c5Connectivity esp32c5;
    ESP_ERROR_CHECK(esp32c5.initialize(config));

    connectivity::Esp32c5CoprocessorInfo info{};
    ESP_ERROR_CHECK(esp32c5.coprocessorInfo(info));
    ESP_LOGI(
        kTag,
        "%s firmware %lu.%lu.%lu, project=%s, IDF=%s",
        info.target.data(),
        static_cast<unsigned long>(info.firmware.major),
        static_cast<unsigned long>(info.firmware.minor),
        static_cast<unsigned long>(info.firmware.patch),
        info.project_name.data(),
        info.idf_version.data());

    connectivity::Esp32c5WifiStationConfig wifi{};
    ESP_ERROR_CHECK(
        connectivity::Esp32c5Connectivity::makeWifiStationConfig(
            WIFI_SSID,
            WIFI_PASSWORD,
            wifi));
    ESP_ERROR_CHECK(esp32c5.startWifiStation(wifi));
    ESP_ERROR_CHECK(
        esp32c5.waitForWifiConnection(kWifiConnectionTimeoutMs));
    ESP_LOGI(kTag, "connected to Wi-Fi SSID: %s", WIFI_SSID);
    ESP_ERROR_CHECK(synchronizeAndPrintTime());

    ESP_ERROR_CHECK(esp32c5.stopWifi());
    ESP_ERROR_CHECK(esp32c5.deinitialize());
    ESP_ERROR_CHECK(board.deinitialize());
}
