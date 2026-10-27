#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <esp_err.h>
#include <esp_netif.h>
#include <esp_wifi.h>

namespace connectivity
{
enum class Esp32c5ConnectivityState : std::uint8_t
{
    uninitialized,
    starting,
    ready,
    recovering,
    stopping,
    failed,
};

enum class Esp32c5RecoveryPhase : std::uint8_t
{
    before_transport_restart,
    after_transport_restart,
};

using Esp32c5RecoveryHook = esp_err_t (*)(
    Esp32c5RecoveryPhase phase,
    void *context);

struct Esp32c5FirmwareVersion
{
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};
};

struct Esp32c5SdioConfig
{
    std::uint32_t clock_frequency_khz{40000};
    std::uint16_t transmit_queue_size{20};
    std::uint16_t receive_queue_size{20};
    std::uint8_t slot{1};
    std::uint8_t bus_width{4};
    int clock_gpio{18};
    int command_gpio{19};
    int data0_gpio{14};
    int data1_gpio{15};
    int data2_gpio{16};
    int data3_gpio{17};
    int enable_gpio{54};
};

struct Esp32c5ConnectivityConfig
{
    Esp32c5SdioConfig expected_sdio{};
    Esp32c5FirmwareVersion minimum_firmware{};
    bool validate_sdio_config{true};
    bool require_exact_hosted_version{false};
    bool initialize_system_services{true};
    bool enable_heartbeat{true};
    std::uint32_t heartbeat_interval_seconds{5};
    bool automatic_recovery{true};
    std::uint8_t initial_connect_attempts{3};
    std::uint8_t recovery_attempts{3};
    std::uint32_t retry_delay_ms{1000};
    std::uint32_t recovery_task_stack_size{4096};
    std::uint8_t recovery_task_priority{5};
    Esp32c5RecoveryHook recovery_hook{};
    void *recovery_hook_context{};
};

struct Esp32c5CoprocessorInfo
{
    std::uint32_t chip_id{};
    std::array<char, 16> target{};
    Esp32c5FirmwareVersion firmware{};
    Esp32c5FirmwareVersion hosted_component{};
    bool firmware_matches_hosted_component{};
    std::array<char, 32> project_name{};
    std::array<char, 32> application_version{};
    std::array<char, 32> idf_version{};
};

struct Esp32c5ConnectivityStatus
{
    Esp32c5ConnectivityState state{Esp32c5ConnectivityState::uninitialized};
    esp_err_t last_error{ESP_OK};
    bool transport_up{};
    bool wifi_started{};
    bool wifi_connected{};
    bool bluetooth_controller_enabled{};
    std::uint32_t recovery_count{};
    std::uint32_t wifi_reconnect_count{};
    std::uint32_t last_heartbeat{};
};

struct Esp32c5WifiStationConfig
{
    std::array<char, 33> ssid{};
    std::array<char, 65> password{};
    wifi_auth_mode_t minimum_auth_mode{WIFI_AUTH_WPA2_PSK};
    wifi_ps_type_t power_save{WIFI_PS_MIN_MODEM};
    bool connect_immediately{true};
    bool automatic_reconnect{true};
    // Zero permits unlimited reconnect attempts.
    std::uint16_t maximum_reconnect_attempts{0};
};

class Esp32c5Connectivity
{
public:
    Esp32c5Connectivity() noexcept;
    ~Esp32c5Connectivity();

    Esp32c5Connectivity(const Esp32c5Connectivity &) = delete;
    Esp32c5Connectivity &operator=(const Esp32c5Connectivity &) = delete;

    [[nodiscard]] esp_err_t initialize(
        const Esp32c5ConnectivityConfig &config = {});
    [[nodiscard]] esp_err_t deinitialize();
    [[nodiscard]] esp_err_t recover();

    [[nodiscard]] bool isInitialized() const noexcept;
    [[nodiscard]] Esp32c5ConnectivityStatus status() const noexcept;
    [[nodiscard]] esp_err_t coprocessorInfo(
        Esp32c5CoprocessorInfo &info) const noexcept;
    [[nodiscard]] esp_err_t activeSdioConfig(
        Esp32c5SdioConfig &config) const noexcept;

    [[nodiscard]] esp_err_t startWifiStation(
        const Esp32c5WifiStationConfig &config);
    [[nodiscard]] esp_err_t reconfigureWifiStation(
        const Esp32c5WifiStationConfig &config);
    [[nodiscard]] esp_err_t stopWifi();
    [[nodiscard]] esp_err_t connectWifiStation();
    [[nodiscard]] esp_err_t disconnectWifiStation();
    [[nodiscard]] esp_err_t waitForWifiConnection(
        std::uint32_t timeout_ms) const noexcept;
    [[nodiscard]] esp_err_t scanAccessPoints(
        wifi_ap_record_t *records,
        std::uint16_t &record_count);
    [[nodiscard]] esp_err_t stationAccessPoint(
        wifi_ap_record_t &record) const;
    [[nodiscard]] esp_netif_t *wifiStationNetif() const noexcept;

    // The application must initialize/deinitialize its NimBLE or Bluedroid
    // host stack after/before these controller operations.
    [[nodiscard]] esp_err_t startBluetoothController();
    [[nodiscard]] esp_err_t stopBluetoothController(
        bool release_controller_memory = false);

    [[nodiscard]] static esp_err_t makeWifiStationConfig(
        const char *ssid,
        const char *password,
        Esp32c5WifiStationConfig &config) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char *esp32c5ConnectivityStateName(
    Esp32c5ConnectivityState state) noexcept;
}
