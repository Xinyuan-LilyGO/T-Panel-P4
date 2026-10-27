#include <esp32c5_connectivity/esp32c5_connectivity.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>

#include <esp_event.h>
#include <esp_hosted.h>
#include <esp_hosted_event.h>
#include <esp_hosted_host_fw_ver.h>
#include <esp_hosted_transport_config.h>
#include <esp_log.h>
#include <esp_wifi_default.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs_flash.h>

#if !defined(CONFIG_ESP_HOSTED_ENABLED) ||             \
    !defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) || \
    !defined(CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5)
#error "esp32c5_connectivity requires ESP-Hosted SDIO with ESP32-C5 selected"
#endif

namespace
{
    constexpr char kTag[] = "esp32c5_conn";
    constexpr EventBits_t kWifiConnectedBit = BIT0;
    constexpr EventBits_t kWorkerStoppedBit = BIT1;
    constexpr EventBits_t kWifiScanDoneBit = BIT2;
    constexpr std::uint32_t kWifiScanTimeoutMs = 15000U;

    bool isVersionZero(const connectivity::Esp32c5FirmwareVersion &version) noexcept
    {
        return version.major == 0U && version.minor == 0U && version.patch == 0U;
    }

    int compareVersions(
        const connectivity::Esp32c5FirmwareVersion &left,
        const connectivity::Esp32c5FirmwareVersion &right) noexcept
    {
        if (left.major != right.major)
            return left.major < right.major ? -1 : 1;
        if (left.minor != right.minor)
            return left.minor < right.minor ? -1 : 1;
        if (left.patch != right.patch)
            return left.patch < right.patch ? -1 : 1;
        return 0;
    }

    std::size_t boundedStringLength(
        const char *text,
        std::size_t capacity) noexcept
    {
        std::size_t length{};
        while (length < capacity && text[length] != '\0')
            ++length;
        return length;
    }

    TickType_t millisecondsToTicks(std::uint32_t milliseconds) noexcept
    {
        TickType_t ticks = pdMS_TO_TICKS(milliseconds);
        if (milliseconds != 0U && ticks == 0)
            ticks = 1;
        return ticks;
    }

    void preserveFirstError(esp_err_t candidate, esp_err_t &first_error) noexcept
    {
        if (first_error == ESP_OK && candidate != ESP_OK)
            first_error = candidate;
    }

    class MutexGuard
    {
    public:
        explicit MutexGuard(SemaphoreHandle_t mutex) noexcept
            : mutex_(mutex), locked_(mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY))
        {
        }

        ~MutexGuard()
        {
            if (locked_)
                xSemaphoreGive(mutex_);
        }

        [[nodiscard]] bool locked() const noexcept { return locked_; }

    private:
        SemaphoreHandle_t mutex_{};
        bool locked_{};
    };
}

namespace connectivity
{
    struct Esp32c5Connectivity::Impl
    {
        Esp32c5ConnectivityConfig config{};
        Esp32c5CoprocessorInfo coprocessor_info{};
        Esp32c5SdioConfig active_sdio{};
        Esp32c5WifiStationConfig station_config{};

        SemaphoreHandle_t mutex{};
        EventGroupHandle_t events{};
        TaskHandle_t recovery_task{};
        esp_event_handler_instance_t hosted_event_instance{};
        esp_event_handler_instance_t wifi_event_instance{};
        esp_event_handler_instance_t ip_event_instance{};
        esp_netif_t *station_netif{};

        std::atomic<Esp32c5ConnectivityState> state{
            Esp32c5ConnectivityState::uninitialized};
        std::atomic<int> last_error{ESP_OK};
        std::atomic<bool> initialized{};
        std::atomic<bool> hosted_initialized{};
        std::atomic<bool> transport_up{};
        std::atomic<bool> wifi_initialized{};
        std::atomic<bool> wifi_started{};
        std::atomic<bool> wifi_connected{};
        std::atomic<bool> wifi_scan_active{};
        std::atomic<bool> wifi_stopping{};
        std::atomic<bool> wifi_manual_disconnect{};
        std::atomic<bool> bluetooth_initialized{};
        std::atomic<bool> bluetooth_enabled{};
        std::atomic<bool> stop_worker{};
        std::atomic<std::uint32_t> recovery_count{};
        std::atomic<std::uint32_t> wifi_reconnect_count{};
        std::atomic<std::uint32_t> last_heartbeat{};

        [[nodiscard]] esp_err_t createResources()
        {
            mutex = xSemaphoreCreateMutex();
            events = xEventGroupCreate();
            if (mutex == nullptr || events == nullptr)
            {
                destroyResources();
                return ESP_ERR_NO_MEM;
            }

            if (config.automatic_recovery)
            {
                const BaseType_t created = xTaskCreate(
                    recoveryTaskEntry,
                    "esp32c5_recovery",
                    config.recovery_task_stack_size,
                    this,
                    config.recovery_task_priority,
                    &recovery_task);
                if (created != pdPASS)
                {
                    destroyResources();
                    return ESP_ERR_NO_MEM;
                }
            }
            return ESP_OK;
        }

        void destroyResources() noexcept
        {
            if (recovery_task != nullptr)
            {
                const TaskHandle_t worker = recovery_task;
                stop_worker.store(true);
                xTaskNotifyGive(worker);
                bool stopped = false;
                if (events != nullptr)
                {
                    const EventBits_t bits = xEventGroupWaitBits(
                        events,
                        kWorkerStoppedBit,
                        pdFALSE,
                        pdTRUE,
                        millisecondsToTicks(5000));
                    stopped = (bits & kWorkerStoppedBit) != 0U;
                }
                if (!stopped)
                    vTaskDelete(worker);
                recovery_task = nullptr;
            }
            if (events != nullptr)
            {
                vEventGroupDelete(events);
                events = nullptr;
            }
            if (mutex != nullptr)
            {
                vSemaphoreDelete(mutex);
                mutex = nullptr;
            }
            stop_worker.store(false);
        }

        void setError(esp_err_t error) noexcept
        {
            last_error.store(error);
            if (error != ESP_OK)
                ESP_LOGE(kTag, "operation failed: %s", esp_err_to_name(error));
        }

        void setState(Esp32c5ConnectivityState value) noexcept
        {
            state.store(value);
            ESP_LOGI(kTag, "state=%s", esp32c5ConnectivityStateName(value));
        }

        [[nodiscard]] esp_err_t initializeSystemServices() const noexcept
        {
            if (!config.initialize_system_services)
                return ESP_OK;

            esp_err_t result = nvs_flash_init();
            if (result != ESP_OK)
            {
                ESP_LOGE(
                    kTag,
                    "NVS initialization failed without erasing storage: %s",
                    esp_err_to_name(result));
                return result;
            }

            result = esp_netif_init();
            if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
                return result;

            result = esp_event_loop_create_default();
            return result == ESP_ERR_INVALID_STATE ? ESP_OK : result;
        }

        [[nodiscard]] esp_err_t registerHostedEvents() noexcept
        {
            return esp_event_handler_instance_register(
                ESP_HOSTED_EVENT,
                ESP_EVENT_ANY_ID,
                hostedEventHandler,
                this,
                &hosted_event_instance);
        }

        void unregisterHostedEvents() noexcept
        {
            if (hosted_event_instance != nullptr)
            {
                (void)esp_event_handler_instance_unregister(
                    ESP_HOSTED_EVENT,
                    ESP_EVENT_ANY_ID,
                    hosted_event_instance);
                hosted_event_instance = nullptr;
            }
        }

        [[nodiscard]] esp_err_t readAndValidateCoprocessor()
        {
            Esp32c5CoprocessorInfo info{};
            info.hosted_component = {
                ESP_HOSTED_VERSION_MAJOR_1,
                ESP_HOSTED_VERSION_MINOR_1,
                ESP_HOSTED_VERSION_PATCH_1,
            };

            char target[16]{};
            esp_err_t result = esp_hosted_get_cp_info(
                &info.chip_id,
                target,
                sizeof(target));
            if (result != ESP_OK)
                return result;
            if (std::strcmp(target, "esp32c5") != 0)
            {
                ESP_LOGE(kTag, "expected ESP32-C5 co-processor, received '%s'", target);
                return ESP_ERR_INVALID_RESPONSE;
            }
            // The hosted API may return a full 15-character target without a
            // terminator. Copy the complete fixed-size buffer and force the
            // destination terminator explicitly so -Wstringop-truncation is
            // not triggered and the public array remains a C string.
            std::memcpy(info.target.data(), target, sizeof(target));
            info.target.back() = '\0';

            esp_hosted_coprocessor_fwver_t hosted_version{};
            result = esp_hosted_get_coprocessor_fwversion(&hosted_version);
            if (result != ESP_OK)
                return result;
            info.firmware = {
                hosted_version.major1,
                hosted_version.minor1,
                hosted_version.patch1,
            };
            info.firmware_matches_hosted_component =
                compareVersions(info.firmware, info.hosted_component) == 0;

            if (!isVersionZero(config.minimum_firmware) &&
                compareVersions(info.firmware, config.minimum_firmware) < 0)
            {
                ESP_LOGE(
                    kTag,
                    "co-processor firmware %lu.%lu.%lu is below required %lu.%lu.%lu",
                    static_cast<unsigned long>(info.firmware.major),
                    static_cast<unsigned long>(info.firmware.minor),
                    static_cast<unsigned long>(info.firmware.patch),
                    static_cast<unsigned long>(config.minimum_firmware.major),
                    static_cast<unsigned long>(config.minimum_firmware.minor),
                    static_cast<unsigned long>(config.minimum_firmware.patch));
                return ESP_ERR_INVALID_VERSION;
            }
            if (config.require_exact_hosted_version &&
                !info.firmware_matches_hosted_component)
            {
                return ESP_ERR_INVALID_VERSION;
            }

            esp_hosted_app_desc_t descriptor{};
            result = esp_hosted_get_coprocessor_app_desc(&descriptor);
            if (result == ESP_OK && descriptor.magic_word == ESP_HOSTED_APP_DESC_MAGIC_WORD)
            {
                std::memcpy(
                    info.project_name.data(),
                    descriptor.project_name,
                    info.project_name.size() - 1U);
                std::memcpy(
                    info.application_version.data(),
                    descriptor.version,
                    info.application_version.size() - 1U);
                std::memcpy(
                    info.idf_version.data(),
                    descriptor.idf_ver,
                    info.idf_version.size() - 1U);
            }
            else if (result != ESP_OK)
            {
                ESP_LOGW(kTag, "co-processor application descriptor unavailable: %s", esp_err_to_name(result));
            }

            struct esp_hosted_sdio_config *sdio{};
            const esp_hosted_transport_err_t transport_result =
                esp_hosted_sdio_get_config(&sdio);
            if (transport_result != ESP_TRANSPORT_OK || sdio == nullptr)
                return ESP_ERR_INVALID_STATE;

            Esp32c5SdioConfig actual{};
            actual.clock_frequency_khz = sdio->clock_freq_khz;
            actual.transmit_queue_size = sdio->tx_queue_size;
            actual.receive_queue_size = sdio->rx_queue_size;
            actual.slot = sdio->slot;
            actual.bus_width = sdio->bus_width;
            actual.clock_gpio = sdio->pin_clk.pin;
            actual.command_gpio = sdio->pin_cmd.pin;
            actual.data0_gpio = sdio->pin_d0.pin;
            actual.data1_gpio = sdio->pin_d1.pin;
            actual.data2_gpio = sdio->pin_d2.pin;
            actual.data3_gpio = sdio->pin_d3.pin;
            actual.enable_gpio = sdio->pin_reset.pin;

            if (config.validate_sdio_config)
            {
                const Esp32c5SdioConfig &expected = config.expected_sdio;
                const bool matches =
                    actual.clock_frequency_khz == expected.clock_frequency_khz &&
                    actual.slot == expected.slot &&
                    actual.bus_width == expected.bus_width &&
                    actual.clock_gpio == expected.clock_gpio &&
                    actual.command_gpio == expected.command_gpio &&
                    actual.data0_gpio == expected.data0_gpio &&
                    actual.data1_gpio == expected.data1_gpio &&
                    actual.data2_gpio == expected.data2_gpio &&
                    actual.data3_gpio == expected.data3_gpio &&
                    actual.enable_gpio == expected.enable_gpio;
                if (!matches)
                {
                    ESP_LOGE(kTag, "compiled ESP-Hosted SDIO configuration does not match board wiring");
                    return ESP_ERR_INVALID_STATE;
                }
            }

            coprocessor_info = info;
            active_sdio = actual;
            if (!info.firmware_matches_hosted_component)
            {
                ESP_LOGW(
                    kTag,
                    "ESP-Hosted host %lu.%lu.%lu and co-processor %lu.%lu.%lu differ",
                    static_cast<unsigned long>(info.hosted_component.major),
                    static_cast<unsigned long>(info.hosted_component.minor),
                    static_cast<unsigned long>(info.hosted_component.patch),
                    static_cast<unsigned long>(info.firmware.major),
                    static_cast<unsigned long>(info.firmware.minor),
                    static_cast<unsigned long>(info.firmware.patch));
            }
            return ESP_OK;
        }

        [[nodiscard]] esp_err_t connectAndValidate(std::uint8_t attempts)
        {
            esp_err_t result = ESP_FAIL;
            for (std::uint8_t attempt = 0; attempt < attempts; ++attempt)
            {
                result = static_cast<esp_err_t>(esp_hosted_connect_to_slave());
                if (result == ESP_OK)
                    result = readAndValidateCoprocessor();
                if (result == ESP_OK)
                {
                    transport_up.store(true);
                    return ESP_OK;
                }
                if (attempt + 1U < attempts)
                    vTaskDelay(millisecondsToTicks(config.retry_delay_ms));
            }
            transport_up.store(false);
            return result;
        }

        [[nodiscard]] esp_err_t registerWifiEvents()
        {
            esp_err_t result = esp_event_handler_instance_register(
                WIFI_EVENT,
                ESP_EVENT_ANY_ID,
                wifiEventHandler,
                this,
                &wifi_event_instance);
            if (result != ESP_OK)
                return result;

            result = esp_event_handler_instance_register(
                IP_EVENT,
                IP_EVENT_STA_GOT_IP,
                ipEventHandler,
                this,
                &ip_event_instance);
            if (result != ESP_OK)
            {
                (void)esp_event_handler_instance_unregister(
                    WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    wifi_event_instance);
                wifi_event_instance = nullptr;
            }
            return result;
        }

        void unregisterWifiEvents() noexcept
        {
            if (ip_event_instance != nullptr)
            {
                (void)esp_event_handler_instance_unregister(
                    IP_EVENT,
                    IP_EVENT_STA_GOT_IP,
                    ip_event_instance);
                ip_event_instance = nullptr;
            }
            if (wifi_event_instance != nullptr)
            {
                (void)esp_event_handler_instance_unregister(
                    WIFI_EVENT,
                    ESP_EVENT_ANY_ID,
                    wifi_event_instance);
                wifi_event_instance = nullptr;
            }
        }

        [[nodiscard]] esp_err_t startWifiLocked(
            const Esp32c5WifiStationConfig &wifi_config)
        {
            if (wifi_initialized.load())
                return ESP_ERR_INVALID_STATE;

            const std::size_t ssid_length = boundedStringLength(
                wifi_config.ssid.data(), wifi_config.ssid.size());
            const std::size_t password_length = boundedStringLength(
                wifi_config.password.data(), wifi_config.password.size());
            if ((ssid_length == 0U && wifi_config.connect_immediately) ||
                ssid_length > 32U || password_length > 64U)
                return ESP_ERR_INVALID_ARG;

            station_netif = esp_netif_create_default_wifi_sta();
            if (station_netif == nullptr)
                return ESP_ERR_NO_MEM;

            esp_err_t result = registerWifiEvents();
            if (result != ESP_OK)
            {
                esp_netif_destroy_default_wifi(station_netif);
                station_netif = nullptr;
                return result;
            }

            wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
            result = esp_wifi_init(&init_config);
            if (result != ESP_OK)
            {
                unregisterWifiEvents();
                esp_netif_destroy_default_wifi(station_netif);
                station_netif = nullptr;
                return result;
            }
            wifi_initialized.store(true);

            wifi_config_t native_config{};
            std::memcpy(
                native_config.sta.ssid,
                wifi_config.ssid.data(),
                ssid_length);
            std::memcpy(
                native_config.sta.password,
                wifi_config.password.data(),
                password_length);
            native_config.sta.threshold.authmode = wifi_config.minimum_auth_mode;
            native_config.sta.pmf_cfg.capable = true;
            native_config.sta.pmf_cfg.required = false;

            result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
            if (result == ESP_OK)
                result = esp_wifi_set_mode(WIFI_MODE_STA);
            if (result == ESP_OK && ssid_length != 0U)
                result = esp_wifi_set_config(WIFI_IF_STA, &native_config);
            if (result == ESP_OK)
                result = esp_wifi_set_ps(wifi_config.power_save);
            if (result == ESP_OK)
                result = esp_wifi_start();
            if (result == ESP_OK)
            {
                wifi_started.store(true);
                station_config = wifi_config;
                wifi_reconnect_count.store(0);
                if (wifi_config.connect_immediately)
                    result = esp_wifi_connect();
            }

            if (result != ESP_OK)
                (void)stopWifiLocked(false);
            return result;
        }

        [[nodiscard]] esp_err_t stopWifiLocked(bool preserve_config) noexcept
        {
            wifi_stopping.store(true);
            unregisterWifiEvents();
            esp_err_t first_error = ESP_OK;
            if (wifi_started.load())
                preserveFirstError(esp_wifi_stop(), first_error);
            if (wifi_initialized.load())
                preserveFirstError(esp_wifi_deinit(), first_error);
            if (station_netif != nullptr)
            {
                esp_netif_destroy_default_wifi(station_netif);
                station_netif = nullptr;
            }
            wifi_initialized.store(false);
            wifi_started.store(false);
            wifi_connected.store(false);
            wifi_manual_disconnect.store(false);
            wifi_stopping.store(false);
            wifi_reconnect_count.store(0);
            if (events != nullptr)
                xEventGroupClearBits(events, kWifiConnectedBit);
            if (!preserve_config)
                station_config = {};
            return first_error;
        }

        [[nodiscard]] esp_err_t startBluetoothLocked() noexcept
        {
#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) || \
    defined(CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
            if (bluetooth_enabled.load())
                return ESP_ERR_INVALID_STATE;
            esp_err_t result = esp_hosted_bt_controller_init();
            if (result != ESP_OK)
                return result;
            bluetooth_initialized.store(true);
            result = esp_hosted_bt_controller_enable();
            if (result != ESP_OK)
            {
                (void)esp_hosted_bt_controller_deinit(false);
                bluetooth_initialized.store(false);
                return result;
            }
            bluetooth_enabled.store(true);
            return ESP_OK;
#else
            return ESP_ERR_NOT_SUPPORTED;
#endif
        }

        [[nodiscard]] esp_err_t stopBluetoothLocked(bool release_memory) noexcept
        {
            esp_err_t first_error = ESP_OK;
            if (bluetooth_enabled.load())
            {
                preserveFirstError(
                    esp_hosted_bt_controller_disable(),
                    first_error);
            }
            if (bluetooth_initialized.load())
            {
                preserveFirstError(
                    esp_hosted_bt_controller_deinit(release_memory),
                    first_error);
            }
            bluetooth_enabled.store(false);
            bluetooth_initialized.store(false);
            return first_error;
        }

        [[nodiscard]] esp_err_t recoverLocked()
        {
            if (!initialized.load())
                return ESP_ERR_INVALID_STATE;
            if (state.load() == Esp32c5ConnectivityState::stopping)
                return ESP_ERR_INVALID_STATE;

            const bool restore_wifi = wifi_started.load();
            const bool restore_bluetooth = bluetooth_enabled.load();
            if (restore_bluetooth && config.recovery_hook == nullptr)
            {
                ESP_LOGE(kTag, "Bluetooth recovery requires a host-stack recovery hook");
                setError(ESP_ERR_INVALID_STATE);
                return ESP_ERR_INVALID_STATE;
            }

            const Esp32c5ConnectivityState previous_state = state.load();
            setState(Esp32c5ConnectivityState::recovering);
            if (restore_bluetooth && config.recovery_hook != nullptr)
            {
                const esp_err_t hook_result = config.recovery_hook(
                    Esp32c5RecoveryPhase::before_transport_restart,
                    config.recovery_hook_context);
                if (hook_result != ESP_OK)
                {
                    setError(hook_result);
                    setState(previous_state);
                    return hook_result;
                }
            }

            if (restore_wifi)
                (void)stopWifiLocked(true);
            if (restore_bluetooth)
                (void)stopBluetoothLocked(false);
            if (config.enable_heartbeat && transport_up.load())
                (void)esp_hosted_configure_heartbeat(false, 1);
            if (hosted_initialized.load())
            {
                (void)esp_hosted_deinit();
                hosted_initialized.store(false);
            }
            transport_up.store(false);

            esp_err_t result = ESP_FAIL;
            for (std::uint8_t attempt = 0; attempt < config.recovery_attempts; ++attempt)
            {
                if (attempt != 0U)
                    vTaskDelay(millisecondsToTicks(config.retry_delay_ms));
                result = static_cast<esp_err_t>(esp_hosted_init());
                if (result == ESP_OK)
                {
                    hosted_initialized.store(true);
                    result = connectAndValidate(1);
                }
                if (result == ESP_OK)
                    break;
                if (hosted_initialized.load())
                {
                    (void)esp_hosted_deinit();
                    hosted_initialized.store(false);
                }
            }

            if (result == ESP_OK && config.enable_heartbeat)
            {
                result = esp_hosted_configure_heartbeat(
                    true,
                    config.heartbeat_interval_seconds);
            }
            if (result == ESP_OK && restore_bluetooth)
                result = startBluetoothLocked();
            if (result == ESP_OK && restore_wifi)
                result = startWifiLocked(station_config);
            if (result == ESP_OK && restore_bluetooth &&
                config.recovery_hook != nullptr)
            {
                result = config.recovery_hook(
                    Esp32c5RecoveryPhase::after_transport_restart,
                    config.recovery_hook_context);
            }

            if (result == ESP_OK)
            {
                recovery_count.fetch_add(1U);
                setError(ESP_OK);
                setState(Esp32c5ConnectivityState::ready);
            }
            else
            {
                setError(result);
                setState(Esp32c5ConnectivityState::failed);
            }
            return result;
        }

        void requestRecovery() noexcept
        {
            if (config.automatic_recovery && recovery_task != nullptr && initialized.load())
                xTaskNotifyGive(recovery_task);
        }

        static void recoveryTaskEntry(void *argument)
        {
            auto &self = *static_cast<Impl *>(argument);
            while (true)
            {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                if (self.stop_worker.load())
                    break;

                MutexGuard guard(self.mutex);
                if (guard.locked() && self.initialized.load() &&
                    self.state.load() != Esp32c5ConnectivityState::recovering &&
                    self.state.load() != Esp32c5ConnectivityState::stopping)
                {
                    (void)self.recoverLocked();
                }
            }
            if (self.events != nullptr)
                xEventGroupSetBits(self.events, kWorkerStoppedBit);
            vTaskDelete(nullptr);
        }

        static void hostedEventHandler(
            void *argument,
            esp_event_base_t,
            std::int32_t event_id,
            void *event_data)
        {
            auto &self = *static_cast<Impl *>(argument);
            switch (event_id)
            {
            case ESP_HOSTED_EVENT_TRANSPORT_UP:
                self.transport_up.store(true);
                break;
            case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
            case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
                self.transport_up.store(false);
                if (self.state.load() != Esp32c5ConnectivityState::recovering &&
                    self.state.load() != Esp32c5ConnectivityState::stopping)
                {
                    self.last_error.store(ESP_FAIL);
                    self.setState(Esp32c5ConnectivityState::failed);
                    self.requestRecovery();
                }
                break;
            case ESP_HOSTED_EVENT_CP_HEARTBEAT:
                if (event_data != nullptr)
                {
                    const auto *heartbeat =
                        static_cast<const esp_hosted_event_heartbeat_t *>(event_data);
                    self.last_heartbeat.store(heartbeat->heartbeat);
                }
                break;
            default:
                break;
            }
        }

        static void wifiEventHandler(
            void *argument,
            esp_event_base_t,
            std::int32_t event_id,
            void *event_data)
        {
            auto &self = *static_cast<Impl *>(argument);
            if (event_id == WIFI_EVENT_SCAN_DONE)
            {
                if (self.events != nullptr)
                    xEventGroupSetBits(self.events, kWifiScanDoneBit);
                return;
            }
            if (event_id != WIFI_EVENT_STA_DISCONNECTED || self.wifi_stopping.load())
                return;

            if (event_data != nullptr)
            {
                const auto *disconnected =
                    static_cast<const wifi_event_sta_disconnected_t *>(event_data);
                ESP_LOGW(
                    kTag,
                    "Wi-Fi disconnected: reason=%u, ssid=%s",
                    static_cast<unsigned>(disconnected->reason),
                    self.station_config.ssid.data());
            }

            self.wifi_connected.store(false);
            if (self.events != nullptr)
                xEventGroupClearBits(self.events, kWifiConnectedBit);
            if (self.wifi_manual_disconnect.exchange(false))
                return;
            if (!self.station_config.automatic_reconnect)
                return;

            const std::uint32_t attempts = self.wifi_reconnect_count.load();
            const std::uint16_t maximum =
                self.station_config.maximum_reconnect_attempts;
            if (maximum != 0U && attempts >= maximum)
            {
                self.last_error.store(ESP_ERR_TIMEOUT);
                return;
            }

            const esp_err_t result = esp_wifi_connect();
            if (result == ESP_OK)
                self.wifi_reconnect_count.fetch_add(1U);
            else
            {
                self.last_error.store(result);
                self.requestRecovery();
            }
        }

        static void ipEventHandler(
            void *argument,
            esp_event_base_t,
            std::int32_t event_id,
            void *)
        {
            if (event_id != IP_EVENT_STA_GOT_IP)
                return;
            auto &self = *static_cast<Impl *>(argument);
            self.wifi_connected.store(true);
            self.wifi_reconnect_count.store(0);
            if (self.events != nullptr)
                xEventGroupSetBits(self.events, kWifiConnectedBit);
        }
    };

    Esp32c5Connectivity::Esp32c5Connectivity() noexcept
        : impl_(new (std::nothrow) Impl{})
    {
    }

    Esp32c5Connectivity::~Esp32c5Connectivity()
    {
        if (impl_ != nullptr)
        {
            if (impl_->initialized.load())
            {
                if (impl_->bluetooth_enabled.load())
                {
                    ESP_LOGW(
                        kTag,
                        "forcing Bluetooth Controller shutdown during destruction");
                    (void)stopBluetoothController(false);
                }
                (void)deinitialize();
            }
            impl_->destroyResources();
        }
    }

    esp_err_t Esp32c5Connectivity::initialize(
        const Esp32c5ConnectivityConfig &config)
    {
        if (impl_ == nullptr)
            return ESP_ERR_NO_MEM;
        if (impl_->initialized.load() || impl_->mutex != nullptr)
            return ESP_ERR_INVALID_STATE;
        if (config.initial_connect_attempts == 0U ||
            (config.automatic_recovery && config.recovery_attempts == 0U) ||
            config.retry_delay_ms == 0U ||
            config.recovery_task_stack_size < 2048U ||
            config.recovery_task_priority >= configMAX_PRIORITIES ||
            (config.enable_heartbeat &&
             (config.heartbeat_interval_seconds == 0U ||
              config.heartbeat_interval_seconds > 86400U)))
        {
            return ESP_ERR_INVALID_ARG;
        }

        impl_->config = config;
        esp_err_t result = impl_->createResources();
        if (result != ESP_OK)
            return result;

        {
            MutexGuard guard(impl_->mutex);
            if (!guard.locked())
                result = ESP_ERR_TIMEOUT;
            else
            {
                impl_->setState(Esp32c5ConnectivityState::starting);
                result = impl_->initializeSystemServices();
                if (result == ESP_OK)
                    result = impl_->registerHostedEvents();
                if (result == ESP_OK)
                {
                    result = static_cast<esp_err_t>(esp_hosted_init());
                    if (result == ESP_OK)
                        impl_->hosted_initialized.store(true);
                }
                if (result == ESP_OK)
                    result = impl_->connectAndValidate(config.initial_connect_attempts);
                if (result == ESP_OK && config.enable_heartbeat)
                {
                    result = esp_hosted_configure_heartbeat(
                        true,
                        config.heartbeat_interval_seconds);
                }

                if (result == ESP_OK)
                {
                    impl_->initialized.store(true);
                    impl_->setError(ESP_OK);
                    impl_->setState(Esp32c5ConnectivityState::ready);
                    ESP_LOGI(
                        kTag,
                        "ESP32-C5 connected over SDIO: firmware %lu.%lu.%lu",
                        static_cast<unsigned long>(impl_->coprocessor_info.firmware.major),
                        static_cast<unsigned long>(impl_->coprocessor_info.firmware.minor),
                        static_cast<unsigned long>(impl_->coprocessor_info.firmware.patch));
                }
                else
                {
                    if (impl_->hosted_initialized.load())
                    {
                        (void)esp_hosted_deinit();
                        impl_->hosted_initialized.store(false);
                    }
                    impl_->unregisterHostedEvents();
                    impl_->setError(result);
                    impl_->setState(Esp32c5ConnectivityState::failed);
                }
            }
        }

        if (result != ESP_OK)
            impl_->destroyResources();
        return result;
    }

    esp_err_t Esp32c5Connectivity::deinitialize()
    {
        if (impl_ == nullptr || !impl_->initialized.load())
            return ESP_ERR_INVALID_STATE;
        if (impl_->bluetooth_enabled.load())
        {
            ESP_LOGE(kTag, "deinitialize Bluetooth host stack, then stop its controller first");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t first_error = ESP_OK;
        {
            MutexGuard guard(impl_->mutex);
            if (!guard.locked())
                return ESP_ERR_TIMEOUT;

            impl_->setState(Esp32c5ConnectivityState::stopping);
            if (impl_->wifi_initialized.load())
                preserveFirstError(impl_->stopWifiLocked(false), first_error);
            if (impl_->config.enable_heartbeat && impl_->transport_up.load())
            {
                preserveFirstError(
                    esp_hosted_configure_heartbeat(false, 1),
                    first_error);
            }
            if (impl_->hosted_initialized.load())
            {
                preserveFirstError(
                    static_cast<esp_err_t>(esp_hosted_deinit()),
                    first_error);
                impl_->hosted_initialized.store(false);
            }
            impl_->unregisterHostedEvents();
            impl_->transport_up.store(false);
            impl_->initialized.store(false);
            impl_->setError(first_error);
            impl_->setState(Esp32c5ConnectivityState::uninitialized);
        }
        impl_->destroyResources();
        return first_error;
    }

    esp_err_t Esp32c5Connectivity::recover()
    {
        if (impl_ == nullptr || !impl_->initialized.load())
            return ESP_ERR_INVALID_STATE;
        MutexGuard guard(impl_->mutex);
        return guard.locked() ? impl_->recoverLocked() : ESP_ERR_TIMEOUT;
    }

    bool Esp32c5Connectivity::isInitialized() const noexcept
    {
        return impl_ != nullptr && impl_->initialized.load();
    }

    Esp32c5ConnectivityStatus Esp32c5Connectivity::status() const noexcept
    {
        Esp32c5ConnectivityStatus value{};
        if (impl_ == nullptr)
        {
            value.last_error = ESP_ERR_NO_MEM;
            value.state = Esp32c5ConnectivityState::failed;
            return value;
        }
        value.state = impl_->state.load();
        value.last_error = static_cast<esp_err_t>(impl_->last_error.load());
        value.transport_up = impl_->transport_up.load();
        value.wifi_started = impl_->wifi_started.load();
        value.wifi_connected = impl_->wifi_connected.load();
        value.bluetooth_controller_enabled = impl_->bluetooth_enabled.load();
        value.recovery_count = impl_->recovery_count.load();
        value.wifi_reconnect_count = impl_->wifi_reconnect_count.load();
        value.last_heartbeat = impl_->last_heartbeat.load();
        return value;
    }

    esp_err_t Esp32c5Connectivity::coprocessorInfo(
        Esp32c5CoprocessorInfo &info) const noexcept
    {
        if (impl_ == nullptr || !impl_->initialized.load())
            return ESP_ERR_INVALID_STATE;
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        info = impl_->coprocessor_info;
        return ESP_OK;
    }

    esp_err_t Esp32c5Connectivity::activeSdioConfig(
        Esp32c5SdioConfig &config) const noexcept
    {
        if (impl_ == nullptr || !impl_->initialized.load())
            return ESP_ERR_INVALID_STATE;
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        config = impl_->active_sdio;
        return ESP_OK;
    }

    esp_err_t Esp32c5Connectivity::startWifiStation(
        const Esp32c5WifiStationConfig &config)
    {
        if (impl_ == nullptr || !impl_->initialized.load() ||
            impl_->state.load() != Esp32c5ConnectivityState::ready)
        {
            return ESP_ERR_INVALID_STATE;
        }
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        const esp_err_t result = impl_->startWifiLocked(config);
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::stopWifi()
    {
        if (impl_ == nullptr || !impl_->initialized.load() ||
            !impl_->wifi_initialized.load())
        {
            return ESP_ERR_INVALID_STATE;
        }
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        const esp_err_t result = impl_->stopWifiLocked(false);
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::reconfigureWifiStation(
        const Esp32c5WifiStationConfig &wifi_config)
    {
        if (impl_ == nullptr || !impl_->initialized.load() ||
            !impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;

        const std::size_t ssid_length = boundedStringLength(
            wifi_config.ssid.data(), wifi_config.ssid.size());
        const std::size_t password_length = boundedStringLength(
            wifi_config.password.data(), wifi_config.password.size());
        if (ssid_length == 0U || ssid_length > 32U || password_length > 64U)
            return ESP_ERR_INVALID_ARG;

        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;

        wifi_config_t native_config{};
        std::memcpy(native_config.sta.ssid, wifi_config.ssid.data(), ssid_length);
        std::memcpy(native_config.sta.password, wifi_config.password.data(), password_length);
        native_config.sta.threshold.authmode = wifi_config.minimum_auth_mode;
        native_config.sta.pmf_cfg.capable = true;
        native_config.sta.pmf_cfg.required = false;

        esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &native_config);
        if (result == ESP_OK)
            result = esp_wifi_set_ps(wifi_config.power_save);
        if (result == ESP_OK)
        {
            impl_->wifi_manual_disconnect.store(false);
            result = esp_wifi_disconnect();
        }
        if (result == ESP_OK)
        {
            impl_->station_config = wifi_config;
            impl_->wifi_reconnect_count.store(0);
            result = esp_wifi_connect();
        }
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::connectWifiStation()
    {
        if (impl_ == nullptr || !impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        if (!impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;
        impl_->wifi_manual_disconnect.store(false);
        const esp_err_t result = esp_wifi_connect();
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::disconnectWifiStation()
    {
        if (impl_ == nullptr || !impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        if (!impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;
        impl_->wifi_manual_disconnect.store(true);
        const esp_err_t result = esp_wifi_disconnect();
        if (result != ESP_OK)
            impl_->wifi_manual_disconnect.store(false);
        impl_->wifi_connected.store(false);
        if (impl_->events != nullptr)
            xEventGroupClearBits(impl_->events, kWifiConnectedBit);
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::waitForWifiConnection(
        std::uint32_t timeout_ms) const noexcept
    {
        if (impl_ == nullptr || !impl_->wifi_started.load() || timeout_ms == 0U)
            return ESP_ERR_INVALID_ARG;
        if (impl_->wifi_connected.load())
            return ESP_OK;
        const EventBits_t bits = xEventGroupWaitBits(
            impl_->events,
            kWifiConnectedBit,
            pdFALSE,
            pdTRUE,
            millisecondsToTicks(timeout_ms));
        return (bits & kWifiConnectedBit) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    esp_err_t Esp32c5Connectivity::scanAccessPoints(
        wifi_ap_record_t *records,
        std::uint16_t &record_count)
    {
        if (impl_ == nullptr || !impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;
        if (records == nullptr || record_count == 0U)
            return ESP_ERR_INVALID_ARG;

        if (impl_->events == nullptr)
            return ESP_ERR_INVALID_STATE;

        {
            MutexGuard guard(impl_->mutex);
            if (!guard.locked())
                return ESP_ERR_TIMEOUT;
            if (impl_->wifi_scan_active.exchange(true))
                return ESP_ERR_INVALID_STATE;

            xEventGroupClearBits(impl_->events, kWifiScanDoneBit);
            /* Start asynchronously so unrelated host tasks do not wait on a
             * remote blocking scan RPC. AP-info polling is suppressed while
             * the scan is active, then the result fetch is serialized. */
            const esp_err_t result = esp_wifi_scan_start(nullptr, false);
            if (result != ESP_OK)
            {
                impl_->wifi_scan_active.store(false);
                impl_->setError(result);
                return result;
            }
        }

        const EventBits_t bits = xEventGroupWaitBits(
            impl_->events,
            kWifiScanDoneBit,
            pdTRUE,
            pdFALSE,
            millisecondsToTicks(kWifiScanTimeoutMs));
        if ((bits & kWifiScanDoneBit) == 0U)
        {
            impl_->wifi_scan_active.store(false);
            impl_->setError(ESP_ERR_WIFI_TIMEOUT);
            return ESP_ERR_WIFI_TIMEOUT;
        }

        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
        {
            impl_->wifi_scan_active.store(false);
            return ESP_ERR_TIMEOUT;
        }

        std::uint16_t count = record_count;
        const esp_err_t result = esp_wifi_scan_get_ap_records(&count, records);
        impl_->wifi_scan_active.store(false);
        if (result == ESP_OK)
            record_count = count;
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::stationAccessPoint(
        wifi_ap_record_t &record) const
    {
        if (impl_ == nullptr || !impl_->wifi_started.load())
            return ESP_ERR_INVALID_STATE;
        if (impl_->wifi_scan_active.load())
            return ESP_ERR_NOT_FINISHED;
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        if (impl_->wifi_scan_active.load())
            return ESP_ERR_NOT_FINISHED;
        return esp_wifi_sta_get_ap_info(&record);
    }

    esp_netif_t *Esp32c5Connectivity::wifiStationNetif() const noexcept
    {
        return impl_ != nullptr && impl_->wifi_started.load()
                   ? impl_->station_netif
                   : nullptr;
    }

    esp_err_t Esp32c5Connectivity::startBluetoothController()
    {
        if (impl_ == nullptr || !impl_->initialized.load() ||
            impl_->state.load() != Esp32c5ConnectivityState::ready)
        {
            return ESP_ERR_INVALID_STATE;
        }
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        const esp_err_t result = impl_->startBluetoothLocked();
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::stopBluetoothController(
        bool release_controller_memory)
    {
        if (impl_ == nullptr || !impl_->bluetooth_initialized.load())
            return ESP_ERR_INVALID_STATE;
        MutexGuard guard(impl_->mutex);
        if (!guard.locked())
            return ESP_ERR_TIMEOUT;
        const esp_err_t result = impl_->stopBluetoothLocked(
            release_controller_memory);
        impl_->setError(result);
        return result;
    }

    esp_err_t Esp32c5Connectivity::makeWifiStationConfig(
        const char *ssid,
        const char *password,
        Esp32c5WifiStationConfig &config) noexcept
    {
        if (ssid == nullptr || password == nullptr)
            return ESP_ERR_INVALID_ARG;
        const std::size_t ssid_length = std::strlen(ssid);
        const std::size_t password_length = std::strlen(password);
        if (ssid_length == 0U || ssid_length > 32U || password_length > 64U)
            return ESP_ERR_INVALID_ARG;

        Esp32c5WifiStationConfig value{};
        std::memcpy(value.ssid.data(), ssid, ssid_length);
        std::memcpy(value.password.data(), password, password_length);
        config = value;
        return ESP_OK;
    }

    const char *esp32c5ConnectivityStateName(
        Esp32c5ConnectivityState state) noexcept
    {
        switch (state)
        {
        case Esp32c5ConnectivityState::uninitialized:
            return "uninitialized";
        case Esp32c5ConnectivityState::starting:
            return "starting";
        case Esp32c5ConnectivityState::ready:
            return "ready";
        case Esp32c5ConnectivityState::recovering:
            return "recovering";
        case Esp32c5ConnectivityState::stopping:
            return "stopping";
        case Esp32c5ConnectivityState::failed:
            return "failed";
        }
        return "unknown";
    }
}
