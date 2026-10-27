#include "wifi_control.h"

#include "service_context.hpp"

#include <cstdint>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>

#define WIFI_SSID "LilyGo-AABB"   // Your_WIFI_SSID
#define WIFI_PASS "xinyuandianzi" // Your_WIFI_PASSWORD

namespace
{
    connectivity::Esp32c5Connectivity *radio()
    {
        return factory::services::connectivity();
    }

    connectivity::Esp32c5WifiStationConfig &wifi_config()
    {
        static connectivity::Esp32c5WifiStationConfig fallback{};
        const auto &context = factory::services::context();
        return context.wifi_config != nullptr ? *context.wifi_config : fallback;
    }

    bool &wifi_config_valid()
    {
        static bool fallback = false;
        const auto &context = factory::services::context();
        return context.wifi_config_valid != nullptr ? *context.wifi_config_valid : fallback;
    }

    bool connectivity_ready()
    {
        const auto *connectivity = radio();
        const auto &context = factory::services::context();
        if (connectivity == nullptr || !connectivity->isInitialized())
            return false;
        return context.connectivity_result == nullptr ||
               *context.connectivity_result == ESP_OK;
    }
}

extern "C" bool wifi_control_is_enabled(void)
{
    const auto *connectivity = radio();
    return connectivity != nullptr && connectivity->status().wifi_started;
}

extern "C" esp_err_t wifi_control_get_status(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;

    const auto *connectivity = radio();
    if (connectivity == nullptr)
    {
        std::snprintf(buffer, buffer_size, "WIFI OFF");
        return ESP_OK;
    }

    const auto status = connectivity->status();
    if (!status.wifi_started)
    {
        std::snprintf(buffer, buffer_size, "WIFI OFF");
    }
    else if (status.wifi_connected)
    {
        const char *ssid = wifi_config().ssid.data();
        if (ssid[0] != '\0')
            std::snprintf(buffer, buffer_size, "CONNECTED // %s", ssid);
        else
            std::snprintf(buffer, buffer_size, "CONNECTED");
    }
    else
    {
        std::snprintf(buffer, buffer_size, "WIFI ON // DISCONNECTED");
    }
    return ESP_OK;
}

extern "C" esp_err_t wifi_control_set_enabled(bool enabled)
{
    auto *connectivity = radio();
    if (!connectivity_ready())
        return ESP_ERR_INVALID_STATE;

    const auto status = connectivity->status();
    if (enabled == status.wifi_started)
        return ESP_OK;
    if (!enabled)
        return connectivity->stopWifi();

    connectivity::Esp32c5WifiStationConfig config = wifi_config();
    if (!wifi_config_valid())
    {
        const esp_err_t result = connectivity::Esp32c5Connectivity::makeWifiStationConfig(
            WIFI_SSID, WIFI_PASS, config);
        if (result != ESP_OK)
            return result;
        config.automatic_reconnect = true;
        config.maximum_reconnect_attempts = 5;
    }
    config.connect_immediately = false;
    wifi_config() = config;
    wifi_config_valid() = true;

    esp_err_t start_result = ESP_ERR_WIFI_NOT_INIT;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        vTaskDelay(pdMS_TO_TICKS(250));
        start_result = connectivity->startWifiStation(config);
        if (start_result == ESP_OK)
            break;
        if (start_result != ESP_ERR_INVALID_STATE &&
            start_result != ESP_ERR_WIFI_NOT_INIT)
            break;
        /* A stop/start cycle can leave the hosted remote Wi-Fi stack in a
         * partially deinitialized state. Rebuild the hosted transport before
         * trying esp_wifi_init again. */
        (void)connectivity->recover();
    }
    return start_result;
}

extern "C" esp_err_t wifi_control_scan(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;
    auto *connectivity = radio();
    if (connectivity == nullptr || !connectivity_ready())
        return ESP_ERR_INVALID_STATE;
    if (!connectivity->status().wifi_started)
    {
        const esp_err_t result = wifi_control_set_enabled(true);
        if (result != ESP_OK)
            return result;
    }

    wifi_ap_record_t records[16]{};
    std::uint16_t count = static_cast<std::uint16_t>(sizeof(records) / sizeof(records[0]));
    const esp_err_t result = connectivity->scanAccessPoints(records, count);
    if (result != ESP_OK)
        return result;

    size_t used = 0U;
    for (std::uint16_t index = 0; index < count; ++index)
    {
        const char *ssid = reinterpret_cast<const char *>(records[index].ssid);
        if (ssid[0] == '\0')
            continue;
        const int written = std::snprintf(
            buffer + used, buffer_size - used, "%.*s\t%d dBm%s\n",
            static_cast<int>(sizeof(records[index].ssid)), ssid,
            records[index].rssi,
            records[index].authmode == WIFI_AUTH_OPEN ? "  OPEN" : "");
        if (written < 0 || static_cast<size_t>(written) >= buffer_size - used)
            break;
        used += static_cast<size_t>(written);
    }
    if (used == 0U)
        std::snprintf(buffer, buffer_size, "NO NETWORKS FOUND");
    return ESP_OK;
}

extern "C" esp_err_t wifi_control_connect(const char *ssid, const char *password)
{
    if (ssid == nullptr || password == nullptr)
        return ESP_ERR_INVALID_ARG;
    auto *connectivity = radio();
    if (connectivity == nullptr || !connectivity_ready())
        return ESP_ERR_INVALID_STATE;

    connectivity::Esp32c5WifiStationConfig config{};
    const esp_err_t config_result =
        connectivity::Esp32c5Connectivity::makeWifiStationConfig(ssid, password, config);
    if (config_result != ESP_OK)
        return config_result;
    config.automatic_reconnect = true;
    config.maximum_reconnect_attempts = 5;
    if (connectivity->status().wifi_started)
    {
        const esp_err_t result = connectivity->reconfigureWifiStation(config);
        if (result == ESP_OK)
        {
            wifi_config() = config;
            wifi_config_valid() = true;
        }
        return result;
    }

    config.connect_immediately = true;
    esp_err_t start_result = ESP_ERR_WIFI_NOT_INIT;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        vTaskDelay(pdMS_TO_TICKS(250));
        start_result = connectivity->startWifiStation(config);
        if (start_result == ESP_OK)
            break;
        if (start_result != ESP_ERR_INVALID_STATE &&
            start_result != ESP_ERR_WIFI_NOT_INIT)
            break;
        (void)connectivity->recover();
    }
    if (start_result == ESP_OK)
    {
        wifi_config() = config;
        wifi_config_valid() = true;
    }
    return start_result;
}
