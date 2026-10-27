#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_codec_dev.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <sdmmc_cmd.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <bsp/t_panel_p4_standard_pins.hpp>
#include <esp32c5_connectivity/esp32c5_connectivity.hpp>
#include <ui/core/standard_lvgl_port.hpp>
#include <ui/core/status_bar.h>
#include <ui/core/ui.h>
#include <app/services/bluetooth_control.h>
#include <app/services/service_context.hpp>

namespace
{
    using bsp::t_panel_p4_standard::AudioCodecChip;
    using bsp::t_panel_p4_standard::AudioMicrophoneGain;
    using bsp::t_panel_p4_standard::AudioWordLength;
    using bsp::t_panel_p4_standard::Board;
    using bsp::t_panel_p4_standard::MusicEvent;
    using bsp::t_panel_p4_standard::MusicEventType;
    using bsp::t_panel_p4_standard::MusicPlaybackConfig;
    using bsp::t_panel_p4_standard::MusicState;

    constexpr char kTag[] = "factory";

    struct Application
    {
        bsp::t_panel_p4_standard::Board board;
        connectivity::Esp32c5Connectivity esp32c5;
        connectivity::Esp32c5ConnectivityConfig esp32c5_config{};
        connectivity::Esp32c5WifiStationConfig wifi_config{};
        bool wifi_config_valid{};
        esp_err_t axp517_result{ESP_ERR_INVALID_STATE};
        esp_err_t wifi_result{ESP_ERR_INVALID_STATE};
        esp_err_t lora_result{ESP_ERR_INVALID_STATE};
        esp_err_t esp32c5_result{ESP_ERR_INVALID_STATE};
        esp_err_t sd_card_result{ESP_ERR_INVALID_STATE};
        esp_err_t audio_result{ESP_ERR_INVALID_STATE};
    };

    Application application;

    void logHeapSnapshot(const char *stage)
    {
        struct HeapType
        {
            const char *name;
            std::uint32_t capabilities;
        };
        constexpr HeapType heap_types[] = {
            {"INTERNAL", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
            {"DMA", MALLOC_CAP_DMA | MALLOC_CAP_8BIT},
            {"SPIRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT},
        };

        ESP_LOGI(kTag, "heap snapshot: %s", stage);
        for (const HeapType &heap : heap_types)
        {
            const std::uint32_t total = heap_caps_get_total_size(heap.capabilities);
            const std::uint32_t free = heap_caps_get_free_size(heap.capabilities);
            const std::uint32_t used = total > free ? total - free : 0;
            // Print tenths of a percent using integer arithmetic; this avoids
            // requiring printf floating-point support in ESP-IDF.
            const std::uint32_t usage_tenths = total != 0
                                                   ? static_cast<std::uint32_t>(
                                                         (static_cast<std::uint64_t>(used) * 1000U) / total)
                                                   : 0;
            ESP_LOGI(kTag,
                     "  %-8s total=%u, free=%u, used=%u, usage=%u.%u%%, largest=%u, minimum=%u",
                     heap.name,
                     total,
                     free,
                     used,
                     usage_tenths / 10U,
                     usage_tenths % 10U,
                     heap_caps_get_largest_free_block(heap.capabilities),
                     heap_caps_get_minimum_free_size(heap.capabilities));
        }
    }

    void heapMonitorTask(void *argument)
    {
        (void)argument;
        for (;;)
        {
            logHeapSnapshot("runtime (every 5s)");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }

    void logDeviceResult(const char *name, esp_err_t result)
    {
        if (result == ESP_OK)
        {
            ESP_LOGI(kTag, "%s initialization: OK", name);
        }
        else
        {
            ESP_LOGE(
                kTag,
                "%s initialization: FAILED (%s)",
                name,
                esp_err_to_name(result));
        }
    }

    void provideStatusBar(Application &app, status_info_t &status)
    {
        const connectivity::Esp32c5ConnectivityStatus wifi_status =
            app.esp32c5.status();
        status.wifi_connected = wifi_status.wifi_connected;
        status.wifi_rssi_dbm = -127;
        if (status.wifi_connected)
        {
            wifi_ap_record_t ap_info{};
            if (app.esp32c5.stationAccessPoint(ap_info) == ESP_OK)
                status.wifi_rssi_dbm = ap_info.rssi;
        }
        status.bluetooth_enabled = bluetooth_control_is_enabled();
        status.bluetooth_connected = bluetooth_control_is_connected();
        status.battery_percent = -1;
        status.battery_charging = false;

        if (app.board.isPowerInitialized())
        {
            std::uint8_t battery_percent{};
            if (app.board.readBatteryPercentage(battery_percent) == ESP_OK)
                status.battery_percent = battery_percent;

            pdl::charge::Axp517PowerStatus power_status{};
            if (app.board.readPowerStatus(power_status) == ESP_OK)
                status.battery_charging = pdl::charge::isBatteryCharging(power_status);
        }
    }

    [[maybe_unused]] void provideStatusBarCallback(status_info_t *status, void *context)
    {
        if (status == nullptr || context == nullptr)
            return;
        provideStatusBar(*static_cast<Application *>(context), *status);
    }

    void initializeDevices(Application &app)
    {
        app.axp517_result = app.board.isPowerInitialized()
                                ? ESP_OK
                                : ESP_ERR_INVALID_STATE;
        logDeviceResult("AXP517", app.axp517_result);

        bsp::t_panel_p4_standard::SdCardConfig sd_config{};
        /* Keep the removable-card bus conservative for reliable camera writes. */
        sd_config.maximum_frequency_khz = 20000U;
        app.sd_card_result = app.board.initializeSdCard(sd_config);
        logDeviceResult("SD card", app.sd_card_result);

        /* LoRa is initialized when its page is entered. */
        app.lora_result = ESP_ERR_INVALID_STATE;

        app.esp32c5_config = {};
        app.esp32c5_result = app.esp32c5.initialize(app.esp32c5_config);
        logDeviceResult("ESP32-C5 SDIO", app.esp32c5_result);
        if (app.esp32c5_result == ESP_OK)
        {
            connectivity::Esp32c5CoprocessorInfo info{};
            const esp_err_t info_result = app.esp32c5.coprocessorInfo(info);
            if (info_result == ESP_OK)
            {
                ESP_LOGI(
                    kTag,
                    "ESP32-C5 firmware: %lu.%lu.%lu, project=%s, IDF=%s",
                    static_cast<unsigned long>(info.firmware.major),
                    static_cast<unsigned long>(info.firmware.minor),
                    static_cast<unsigned long>(info.firmware.patch),
                    info.project_name.data(),
                    info.idf_version.data());
            }
            else
            {
                ESP_LOGE(
                    kTag,
                    "ESP32-C5 compatibility query failed: %s",
                    esp_err_to_name(info_result));
            }

            const esp_err_t wifi_config_result =
                connectivity::Esp32c5Connectivity::makeWifiStationConfig(
                    "xinyuandianzi",
                    "AA15994823428",
                    app.wifi_config);
            if (wifi_config_result == ESP_OK)
            {
                app.wifi_config.automatic_reconnect = true;
                app.wifi_config.maximum_reconnect_attempts = 5;
                app.wifi_config_valid = true;
                app.wifi_result = app.esp32c5.startWifiStation(app.wifi_config);
            }
            else
            {
                app.wifi_result = wifi_config_result;
            }
            logDeviceResult("Wi-Fi station", app.wifi_result);
        }

        bsp::t_panel_p4_standard::AudioConfig audio_config{};
        audio_config.codec = AudioCodecChip::es8389;
        audio_config.sample_rate_hz = 48000U;
        audio_config.word_length = AudioWordLength::bits16;
        audio_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        audio_config.output_volume_db = -12.0F;
        audio_config.microphone_gain = AudioMicrophoneGain::db24_5;
        /* Create the codec in full-duplex mode for ES8389 compatibility, then
         * disable the RX channel below. Recording enables that channel when
         * the record page is entered without reopening the codec. */
        audio_config.enable_capture = true;
        audio_config.enable_speaker = true;
        audio_config.no_dac_reference = true;
        audio_config.dma_descriptor_count  = 4;
        audio_config.dma_frames_per_descriptor = 512;
        app.audio_result = app.board.initializeAudio(audio_config);
        if (app.audio_result == ESP_OK)
            app.audio_result = app.board.setAudioCaptureEnabled(false);
        logDeviceResult("audio ES8389", app.audio_result);

        ESP_LOGI(kTag, "camera OV2710 initialization: deferred until Camera page");

        ESP_LOGI(
            kTag,
            "device initialization summary: AXP517=%s, LORA=DEFERRED, ESP32-C5=%s, WIFI=%s, camera=DEFERRED, SD=%s, ES8389=%s",
            esp_err_to_name(app.axp517_result),
            esp_err_to_name(app.lora_result),
            esp_err_to_name(app.esp32c5_result),
            esp_err_to_name(app.wifi_result),
            esp_err_to_name(app.sd_card_result),
            esp_err_to_name(app.audio_result));
    }
}

extern "C" esp_err_t settings_control_read_power(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U || !application.board.isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    std::uint8_t percent{};
    std::uint16_t voltage{}, vbus{};
    std::int16_t current{};
    pdl::charge::Axp517PowerStatus power_status{};
    pdl::charge::Axp517FaultStatus fault_status{};
    if (application.board.readBatteryPercentage(percent) != ESP_OK ||
        application.board.readBatteryVoltageMv(voltage) != ESP_OK ||
        application.board.readBatteryCurrentMa(current) != ESP_OK ||
        application.board.readVbusVoltageMv(vbus) != ESP_OK ||
        application.board.readPowerStatus(power_status) != ESP_OK ||
        application.board.readPowerFaults(fault_status) != ESP_OK)
        return ESP_FAIL;
    const char *direction = "UNKNOWN";
    if (current < 0)
        direction = "DISCHARGING";
    else if (pdl::charge::isBatteryCharging(power_status))
        direction = "CHARGING";
    else
        direction = "STANDBY";
    const char *charge_state = "UNKNOWN";
    switch (power_status.charge_state)
    {
    case pdl::charge::Axp517ChargeState::trickle: charge_state = "TRICKLE"; break;
    case pdl::charge::Axp517ChargeState::precharge: charge_state = "PRECHARGE"; break;
    case pdl::charge::Axp517ChargeState::constant_current: charge_state = "CONSTANT CURRENT"; break;
    case pdl::charge::Axp517ChargeState::constant_voltage: charge_state = "CONSTANT VOLTAGE"; break;
    case pdl::charge::Axp517ChargeState::complete: charge_state = "COMPLETE"; break;
    case pdl::charge::Axp517ChargeState::not_charging: charge_state = "NOT CHARGING"; break;
    default: break;
    }
    const unsigned battery_power_mw = static_cast<unsigned>(voltage) *
                                      static_cast<unsigned>(current < 0 ? -current : current) / 1000U;
    std::snprintf(buffer, buffer_size,
                  "BATTERY\t%u%%\n"
                  "BAT VOLTAGE\t%umV\n"
                  "BAT CURRENT\t%dmA\n"
                  "BAT POWER\t%umW\n"
                  "VBUS VOLTAGE\t%umV\n"
                  "VBUS PRESENT\t%s\n"
                  "BAT PRESENT\t%s\n"
                  "DIRECTION\t%s\n"
                  "CHARGE STATE\t%s\n"
                  "THERMAL LIMIT\t%s\n"
                  "INPUT LIMIT\t%s\n"
                  "FAULT\t%s",
                  static_cast<unsigned>(percent), static_cast<unsigned>(voltage),
                  static_cast<int>(current), battery_power_mw,
                  static_cast<unsigned>(vbus), power_status.vbus_present ? "YES" : "NO",
                  power_status.battery_present ? "YES" : "NO", direction, charge_state,
                  power_status.thermal_regulation ? "YES" : "NO",
                  power_status.input_current_limited || power_status.input_voltage_limited ? "YES" : "NO",
                  fault_status.system_over_voltage || fault_status.battery_under_voltage ? "ACTIVE" : "NONE");
    return ESP_OK;
}

extern "C" esp_err_t settings_control_get_backlight_brightness(std::uint8_t *percent)
{
    if (percent == nullptr)
        return ESP_ERR_INVALID_ARG;
    if (!application.board.isDisplayInitialized())
        return ESP_ERR_INVALID_STATE;
    *percent = application.board.backlightBrightness();
    return ESP_OK;
}

extern "C" esp_err_t settings_control_set_backlight_brightness(std::uint8_t percent)
{
    return application.board.setBacklightBrightness(percent);
}

extern "C" bool settings_control_is_sd_msc_enabled(void)
{
    return application.board.isUsbMscInitialized();
}

extern "C" esp_err_t settings_control_read_sd_info(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;
    const sdmmc_card_t *card = application.board.sdCard();
    if (card == nullptr)
        return ESP_ERR_INVALID_STATE;

    const char *type = card->is_mmc ? "MMC" : (card->is_sdio ? "SDIO" : "SD");
    const char *class_name = "SDSC";
    if (!card->is_mmc && !card->is_sdio && card->csd.capacity >= 4 * 1024 * 1024)
        class_name = "SDHC/SDXC";
    const std::uint64_t capacity_bytes =
        static_cast<std::uint64_t>(card->csd.capacity) *
        static_cast<std::uint64_t>(card->csd.sector_size);
    const std::uint64_t capacity_mb = capacity_bytes / (1024ULL * 1024ULL);
    const unsigned bus_width = 1U << card->log_bus_width;
    const char *access = application.board.isUsbMscInitialized()
                             ? "USB MSC"
                             : (application.board.isSdCardMounted() ? "FAT MOUNT" : "RAW");
    const int written = std::snprintf(
        buffer, buffer_size,
        "TYPE\t%s (%s)\n"
        "NAME\t%.*s\n"
        "CAPACITY\t%llu MB\n"
        "SECTOR SIZE\t%u B\n"
        "BUS WIDTH\t%u-bit\n"
        "FREQUENCY\t%d MHz\n"
        "ACCESS\t%s",
        type, class_name, static_cast<int>(sizeof(card->cid.name)), card->cid.name,
        static_cast<unsigned long long>(capacity_mb),
        static_cast<unsigned>(card->csd.sector_size), bus_width,
        (card->real_freq_khz > 0 ? card->real_freq_khz : static_cast<int>(card->max_freq_khz)) / 1000,
        access);
    if (written < 0 || static_cast<size_t>(written) >= buffer_size)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

namespace
{
    bool appendCheckLine(char *buffer, size_t buffer_size, size_t &used,
                         const char *name, bool passed, const char *detail)
    {
        if (buffer == nullptr || buffer_size == 0U || name == nullptr || detail == nullptr ||
            used >= buffer_size)
            return false;
        const int written = std::snprintf(
            buffer + used, buffer_size - used, "%s\t%s%s\n", name,
            passed ? "OK" : "FAIL", detail[0] == '\0' ? "" : detail);
        if (written < 0 || static_cast<size_t>(written) >= buffer_size - used)
            return false;
        used += static_cast<size_t>(written);
        return true;
    }

    const char *checkErrorDetail(char *buffer, size_t buffer_size, esp_err_t error)
    {
        std::snprintf(buffer, buffer_size, " (%s)", esp_err_to_name(error));
        return buffer;
    }
}

extern "C" esp_err_t settings_control_run_check(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;

    size_t used = 0U;
    unsigned failed = 0U;
    char detail[96];
    bool passed = false;

    // A live panel and frame buffer prove that the display transport and
    // rendering pipeline were initialized. The result remains visible on the
    // screen while this check is running, so no destructive panel reset is
    // needed here.
    passed = application.board.isDisplayInitialized() &&
             application.board.displayPanel() != nullptr &&
             application.board.frameBuffer() != nullptr &&
             application.board.frameBufferCount() > 0U;
    detail[0] = '\0';
    if (!passed)
        checkErrorDetail(detail, sizeof(detail), ESP_ERR_INVALID_STATE);
    if (!appendCheckLine(buffer, buffer_size, used, "DISPLAY", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    auto *touch = application.board.touchController();
    std::array<std::uint8_t, 2> touch_id{};
    pdl::Status touch_status = pdl::Status::not_initialized;
    if (touch != nullptr)
    {
        touch_status = touch->readChipId(touch_id);
        if (pdl::isOk(touch_status))
        {
            pdl::touch::JadardTouchData touch_data{};
            touch_status = touch->readTouchData(touch_data);
        }
    }
    passed = pdl::isOk(touch_status);
    if (passed)
        std::snprintf(detail, sizeof(detail), " (ID %02X%02X)", touch_id[0], touch_id[1]);
    else
        std::snprintf(detail, sizeof(detail), " (%s)", pdl::statusName(touch_status));
    if (!appendCheckLine(buffer, buffer_size, used, "TOUCH", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    pdl::charge::Axp517PowerStatus power_status{};
    const esp_err_t axp_error = application.board.readPowerStatus(power_status);
    passed = axp_error == ESP_OK;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (VBUS %s)",
                      power_status.vbus_present ? "PRESENT" : "ABSENT");
    else
        checkErrorDetail(detail, sizeof(detail), axp_error);
    if (!appendCheckLine(buffer, buffer_size, used, "AXP517", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    esp_err_t lora_error = ESP_OK;
    if (!application.board.isLoraInitialized())
        lora_error = application.board.initializeLora();
    if (lora_error == ESP_OK)
        lora_error = application.board.loraStandby();
    passed = lora_error == ESP_OK;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (LR2021 READY)");
    else
        checkErrorDetail(detail, sizeof(detail), lora_error);
    if (!appendCheckLine(buffer, buffer_size, used, "LORA", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    const esp_codec_dev_handle_t codec = application.board.audioCodec();
    int codec_id_high = 0;
    int codec_id_low = 0;
    int codec_error = codec == nullptr
                          ? -1
                          : esp_codec_dev_read_reg(codec, 0xFD, &codec_id_high);
    if (codec_error == ESP_CODEC_DEV_OK)
        codec_error = esp_codec_dev_read_reg(codec, 0xFE, &codec_id_low);
    passed = codec_error == ESP_CODEC_DEV_OK;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (ID %02X%02X)",
                      codec_id_high & 0xFF, codec_id_low & 0xFF);
    else
        std::snprintf(detail, sizeof(detail), " (CODEC ERROR %d)", codec_error);
    if (!appendCheckLine(buffer, buffer_size, used, "ES8389", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    sdmmc_card_t *card = application.board.sdCard();
    const esp_err_t sd_error = card == nullptr ? ESP_ERR_INVALID_STATE : sdmmc_get_status(card);
    passed = sd_error == ESP_OK;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (%u-bit BUS)",
                      1U << card->log_bus_width);
    else
        checkErrorDetail(detail, sizeof(detail), sd_error);
    if (!appendCheckLine(buffer, buffer_size, used, "SD", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    std::uint16_t camera_id = 0U;
    esp_err_t camera_error = application.board.isCameraInitialized()
                                 ? application.board.readCameraProductId(camera_id)
                                 : application.board.initializeCamera();
    if (camera_error == ESP_OK && !application.board.isCameraInitialized())
        camera_error = ESP_ERR_INVALID_STATE;
    if (camera_error == ESP_OK)
        camera_error = application.board.readCameraProductId(camera_id);
    passed = camera_error == ESP_OK && camera_id == 0x2710U;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (PID 0x%04X)", camera_id);
    else if (camera_error == ESP_OK)
        std::snprintf(detail, sizeof(detail), " (UNEXPECTED PID 0x%04X)", camera_id);
    else
        checkErrorDetail(detail, sizeof(detail), camera_error);
    if (!appendCheckLine(buffer, buffer_size, used, "OV2710", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    connectivity::Esp32c5CoprocessorInfo coprocessor_info{};
    const esp_err_t c5_error = application.esp32c5.isInitialized()
                                   ? application.esp32c5.coprocessorInfo(coprocessor_info)
                                   : ESP_ERR_INVALID_STATE;
    passed = c5_error == ESP_OK;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (FW %lu.%lu.%lu)",
                      static_cast<unsigned long>(coprocessor_info.firmware.major),
                      static_cast<unsigned long>(coprocessor_info.firmware.minor),
                      static_cast<unsigned long>(coprocessor_info.firmware.patch));
    else
        checkErrorDetail(detail, sizeof(detail), c5_error);
    if (!appendCheckLine(buffer, buffer_size, used, "ESP32C5", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    detail[0] = '\0';
    std::snprintf(detail, sizeof(detail), " (%u/%u PASSED)", 8U - failed, 8U);
    if (!appendCheckLine(buffer, buffer_size, used, "SUMMARY", failed == 0U, detail))
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

extern "C" esp_err_t settings_control_set_sd_msc_enabled(bool enabled)
{
    bsp::t_panel_p4_standard::SdCardConfig sd_config{};
    sd_config.maximum_frequency_khz = 10000U;

    if (enabled)
    {
        if (application.board.isUsbMscInitialized())
            return ESP_OK;

        /* USB MSC needs exclusive raw block-device access. The normal
         * application starts with the card mounted through FatFS, so release
         * that owner before handing the card to TinyUSB. */
        if (application.board.isSdCardMounted())
        {
            const esp_err_t unmount_result = application.board.deinitializeSdCard();
            if (unmount_result != ESP_OK)
                return unmount_result;
        }

        const esp_err_t msc_result = application.board.initializeUsbMsc();
        if (msc_result != ESP_OK)
        {
            /* Keep the application usable when TinyUSB or the USB cable is
             * unavailable: restore the normal mounted SD-card state. */
            const esp_err_t restore_result = application.board.initializeSdCard(sd_config);
            if (restore_result != ESP_OK)
                ESP_LOGE(kTag, "restore SD mount after MSC failure failed: %s",
                         esp_err_to_name(restore_result));
        }
        return msc_result;
    }

    const esp_err_t msc_result = application.board.deinitializeUsbMsc();
    if (msc_result != ESP_OK)
        return msc_result;
    if (!application.board.isSdCardInitialized())
        return application.board.initializeSdCard(sd_config);
    return ESP_OK;
}

extern "C" esp_err_t settings_control_enter_ship_mode(void)
{
    const auto progress = [](bsp::t_panel_p4_standard::ShipModeStep step,
                             bool complete, esp_err_t result, void *) {
        settings_ship_progress_update(static_cast<std::uint8_t>(step), complete, result);
        // Let the LVGL task present each state while hardware is still alive.
        if (step != bsp::t_panel_p4_standard::ShipModeStep::display &&
            step != bsp::t_panel_p4_standard::ShipModeStep::finalize)
            vTaskDelay(pdMS_TO_TICKS(150));
        if (!complete && step == bsp::t_panel_p4_standard::ShipModeStep::touch)
            factory::ui::prepareStandardLvglPowerDown(false);
        if (!complete && step == bsp::t_panel_p4_standard::ShipModeStep::axp517)
            factory::ui::prepareStandardLvglPowerDown(true);
    };
    const esp_err_t result = application.board.enterShipMode(100, progress, nullptr);
    if (result != ESP_OK && application.board.isDisplayInitialized())
    {
        if (!application.board.isTouchInitialized())
            (void)application.board.initializeTouch();
        factory::ui::resumeStandardLvglDisplay();
    }
    return result;
}

extern "C" esp_err_t settings_control_enter_deep_sleep(void)
{
    const auto progress = [](std::uint8_t step, bool complete, esp_err_t error, void *) {
        settings_sleep_progress_update(step, complete, error);
        vTaskDelay(pdMS_TO_TICKS(150));
        if (step == 3U && !complete)
            factory::ui::prepareStandardLvglPowerDown(false);
        if (step == 6U && !complete)
            factory::ui::prepareStandardLvglPowerDown(true);
    };

    const bool connectivity_was_initialized = application.esp32c5.isInitialized();
    const bool wifi_was_started = application.esp32c5.status().wifi_started;
    const auto restore_connectivity = [&]() {
        if (!connectivity_was_initialized || application.esp32c5.isInitialized())
            return ESP_OK;

        esp_err_t restore_result = application.esp32c5.initialize(
            application.esp32c5_config);
        if (restore_result == ESP_OK && wifi_was_started)
        {
            restore_result = application.wifi_config_valid
                                 ? application.esp32c5.startWifiStation(
                                       application.wifi_config)
                                 : ESP_ERR_INVALID_STATE;
        }
        application.esp32c5_result = restore_result;
        if (restore_result != ESP_OK)
            ESP_LOGE(kTag, "restore ESP32-C5 after sleep failure failed: %s",
                     esp_err_to_name(restore_result));
        return restore_result;
    };

    esp_err_t result = status_bar_stop_monitor();
    if (result != ESP_OK)
        return result;

    if (connectivity_was_initialized)
    {
        ESP_LOGI(kTag, "Stopping ESP32-C5 Wi-Fi and ESP-Hosted before deep sleep");
        result = application.esp32c5.deinitialize();
        if (result != ESP_OK)
        {
            (void)restore_connectivity();
            status_bar_start_monitor(provideStatusBarCallback, &application);
            return result;
        }
        application.esp32c5_result = ESP_ERR_INVALID_STATE;
    }

    result = application.board.enterDeepSleep(
        bsp::t_panel_p4_standard::DeepSleepConfig{}, progress, nullptr);
    if (result != ESP_OK && application.board.isDisplayInitialized())
    {
        if (!application.board.isTouchInitialized())
            (void)application.board.initializeTouch();
        factory::ui::resumeStandardLvglDisplay();
    }
    if (result != ESP_OK)
    {
        (void)restore_connectivity();
        status_bar_start_monitor(provideStatusBarCallback, &application);
    }
    return result;
}

extern "C" esp_err_t settings_control_enter_light_sleep(void)
{
    const auto progress = [](std::uint8_t step, bool complete, esp_err_t error, void *) {
        settings_sleep_progress_update(step, complete, error);
        vTaskDelay(pdMS_TO_TICKS(150));
        if (step == 6U && !complete)
            factory::ui::prepareStandardLvglPowerDown(true);
        if (step == 8U && complete)
            factory::ui::resumeStandardLvglDisplay();
    };

    const bool connectivity_was_initialized = application.esp32c5.isInitialized();
    const bool wifi_was_started = application.esp32c5.status().wifi_started;
    const auto restore_connectivity = [&]() {
        if (!connectivity_was_initialized || application.esp32c5.isInitialized())
            return ESP_OK;

        esp_err_t restore_result = application.esp32c5.initialize(
            application.esp32c5_config);
        if (restore_result == ESP_OK && wifi_was_started)
        {
            restore_result = application.wifi_config_valid
                                 ? application.esp32c5.startWifiStation(
                                       application.wifi_config)
                                 : ESP_ERR_INVALID_STATE;
        }
        application.esp32c5_result = restore_result;
        if (restore_result != ESP_OK)
            ESP_LOGE(kTag, "restore ESP32-C5 after light sleep failed: %s",
                     esp_err_to_name(restore_result));
        return restore_result;
    };

    esp_err_t result = status_bar_stop_monitor();
    if (result != ESP_OK)
        return result;
    if (connectivity_was_initialized)
    {
        result = application.esp32c5.deinitialize();
        if (result != ESP_OK)
        {
            (void)restore_connectivity();
            status_bar_start_monitor(provideStatusBarCallback, &application);
            return result;
        }
        application.esp32c5_result = ESP_ERR_INVALID_STATE;
    }

    result = application.board.enterLightSleep(
        bsp::t_panel_p4_standard::LightSleepConfig{}, progress, nullptr);
    if (application.board.isDisplayInitialized())
    {
        if (!application.board.isTouchInitialized())
            (void)application.board.initializeTouch();
        factory::ui::resumeStandardLvglDisplay();
    }
    settings_sleep_progress_update(9U, false, ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(150));
    const esp_err_t connectivity_result = restore_connectivity();
    settings_sleep_progress_update(9U, true, connectivity_result);
    if (result == ESP_OK && connectivity_result != ESP_OK)
        result = connectivity_result;
    status_bar_start_monitor(provideStatusBarCallback, &application);
    return result;
}

extern "C" void app_main(void)
{
    logHeapSnapshot("before board initialize");
    ESP_ERROR_CHECK(application.board.initialize());
    logHeapSnapshot("after board initialize");
    bsp::t_panel_p4_standard::DisplayConfig display_config{};
    display_config.frame_buffer_count = 2;
    ESP_ERROR_CHECK(application.board.initializeDisplay(display_config));
    ESP_ERROR_CHECK(application.board.initializeTouch());
    ESP_ERROR_CHECK(application.board.setBacklightBrightness(50));
    logHeapSnapshot("after display initialize");

    initializeDevices(application);
    logHeapSnapshot("after devices initialize");

    factory::services::set_context(application.board, application.esp32c5,
                                   &application.wifi_config,
                                   &application.wifi_config_valid,
                                   &application.esp32c5_result);
    ESP_ERROR_CHECK(factory::ui::initializeStandardLvgl(application.board));
    logHeapSnapshot("after initializeStandardLvgl");
    ESP_ERROR_CHECK(factory_ui_start());
    logHeapSnapshot("after factory_ui_start");

    status_bar_start_monitor(provideStatusBarCallback, &application);
}
