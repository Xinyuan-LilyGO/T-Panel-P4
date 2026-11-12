#include "wifi_control.h"

#include "service_context.hpp"

#include <cstdint>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>

#define WIFI_SSID "LilyGo-AABB"
#define WIFI_PASS "xinyuandianzi"

namespace
{
constexpr TickType_t kReadyPollPeriod = pdMS_TO_TICKS(50);
constexpr TickType_t kReadyTimeout = pdMS_TO_TICKS(3000);

connectivity::Esp32c5Connectivity *radio()
{
    return factory::services::connectivity();
}

connectivity::Esp32c5ConnectivityStatus radioStatus()
{
    const auto *connectivity = radio();
    return connectivity != nullptr
               ? connectivity->status()
               : connectivity::Esp32c5ConnectivityStatus{};
}

bool connectivityReady()
{
    const auto *connectivity = radio();
    const auto &context = factory::services::context();
    if (connectivity == nullptr || !connectivity->isInitialized())
        return false;
    if (context.connectivity_result != nullptr &&
        *context.connectivity_result != ESP_OK)
        return false;

    const auto status = connectivity->status();
    return status.state == connectivity::Esp32c5ConnectivityState::ready &&
           status.transport_up;
}

bool waitForConnectivityReady()
{
    TickType_t elapsed = 0;
    while (!connectivityReady() && elapsed < kReadyTimeout)
    {
        vTaskDelay(kReadyPollPeriod);
        elapsed += kReadyPollPeriod;
    }
    return connectivityReady();
}

connectivity::Esp32c5WifiStationConfig &wifiConfig()
{
    static connectivity::Esp32c5WifiStationConfig fallback{};
    const auto &context = factory::services::context();
    return context.wifi_config != nullptr ? *context.wifi_config : fallback;
}

bool &wifiConfigValid()
{
    static bool fallback = false;
    const auto &context = factory::services::context();
    return context.wifi_config_valid != nullptr
               ? *context.wifi_config_valid
               : fallback;
}

esp_err_t makeDefaultConfig(
    connectivity::Esp32c5WifiStationConfig &config)
{
    const esp_err_t result =
        connectivity::Esp32c5Connectivity::makeWifiStationConfig(
            WIFI_SSID, WIFI_PASS, config);
    if (result != ESP_OK)
        return result;
    config.automatic_reconnect = true;
    config.maximum_reconnect_attempts = 5;
    return ESP_OK;
}
}

extern "C" bool wifi_control_is_enabled(void)
{
    const auto status = radioStatus();
    return status.wifi_started;
}

extern "C" esp_err_t wifi_control_get_status(
    char *buffer,
    size_t buffer_size)
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
    if (!status.transport_up || !status.wifi_started)
    {
        std::snprintf(buffer, buffer_size, "WIFI OFF");
    }
    else if (status.wifi_connected)
    {
        const auto &ssid = wifiConfig().ssid;
        if (ssid[0] != '\0')
            std::snprintf(buffer, buffer_size, "CONNECTED // %s", ssid.data());
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
    if (connectivity == nullptr || !waitForConnectivityReady())
        return ESP_ERR_INVALID_STATE;

    const auto status = connectivity->status();
    if (enabled == status.wifi_started)
        return ESP_OK;
    if (!enabled)
        return connectivity->stopWifi();

    auto config = wifiConfig();
    if (!wifiConfigValid())
    {
        const esp_err_t result = makeDefaultConfig(config);
        if (result != ESP_OK)
            return result;
        wifiConfig() = config;
        wifiConfigValid() = true;
    }
    // Starting the station with connect_immediately=false enables scanning and
    // leaves the explicit connection operation to wifi_control_connect().
    config.connect_immediately = false;
    return connectivity->startWifiStation(config);
}

extern "C" esp_err_t wifi_control_scan(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;

    auto *connectivity = radio();
    if (connectivity == nullptr || !waitForConnectivityReady())
        return ESP_ERR_INVALID_STATE;

    if (!connectivity->status().wifi_started)
    {
        const esp_err_t result = wifi_control_set_enabled(true);
        if (result != ESP_OK)
            return result;
    }

    wifi_ap_record_t records[16]{};
    std::uint16_t count =
        static_cast<std::uint16_t>(sizeof(records) / sizeof(records[0]));
    const esp_err_t result = connectivity->scanAccessPoints(records, count);
    if (result != ESP_OK)
        return result;

    size_t used = 0U;
    for (std::uint16_t index = 0; index < count; ++index)
    {
        const char *ssid =
            reinterpret_cast<const char *>(records[index].ssid);
        if (ssid[0] == '\0')
            continue;

        const int written = std::snprintf(
            buffer + used,
            buffer_size - used,
            "%.*s\t%d dBm%s\n",
            static_cast<int>(sizeof(records[index].ssid)),
            ssid,
            records[index].rssi,
            records[index].authmode == WIFI_AUTH_OPEN ? "  OPEN" : "");
        if (written < 0 ||
            static_cast<size_t>(written) >= buffer_size - used)
            break;
        used += static_cast<size_t>(written);
    }
    if (used == 0U)
        std::snprintf(buffer, buffer_size, "NO NETWORKS FOUND");
    return ESP_OK;
}

extern "C" esp_err_t wifi_control_connect(
    const char *ssid,
    const char *password)
{
    if (ssid == nullptr || password == nullptr)
        return ESP_ERR_INVALID_ARG;

    auto *connectivity = radio();
    if (connectivity == nullptr || !waitForConnectivityReady())
        return ESP_ERR_INVALID_STATE;

    connectivity::Esp32c5WifiStationConfig config{};
    const esp_err_t config_result =
        connectivity::Esp32c5Connectivity::makeWifiStationConfig(
            ssid, password, config);
    if (config_result != ESP_OK)
        return config_result;

    config.automatic_reconnect = true;
    config.maximum_reconnect_attempts = 5;
    config.connect_immediately = true;

    esp_err_t result = ESP_OK;
    if (connectivity->status().wifi_started)
        result = connectivity->reconfigureWifiStation(config);
    else
        result = connectivity->startWifiStation(config);

    if (result == ESP_OK)
    {
        wifiConfig() = config;
        wifiConfigValid() = true;
    }
    return result;
}
