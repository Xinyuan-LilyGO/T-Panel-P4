#include "settings_control.h"
#include "service_context.hpp"
#include "ble_control.h"
#include "record_audio.h"
#include <ui/core/lvgl_port.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <esp_log.h>
#include <sdmmc_cmd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pdl/drivers/touch/gt911/gt911.hpp>

namespace
{
constexpr char kTag[] = "round_settings";
constexpr std::uint16_t kInputCurrentLimitMa = 1500;
constexpr std::uint16_t kInputVoltageLimitMv = 4600;
constexpr std::uint16_t kMinimumSystemVoltageMv = 3500;
constexpr std::uint16_t kChargeVoltageMv = 4200;
constexpr std::uint16_t kChargeCurrentMa = 512;

// Stop the NimBLE host before ESP-Hosted tears down its HCI transport.
struct RadioPause
{
    connectivity::Esp32c5Connectivity *radio{factory::services::connectivity()};
    bool initialized{radio != nullptr && radio->isInitialized()};
    bool wifi{initialized && radio->status().wifi_started};
    bool ble{bluetooth_control_is_enabled()};

    esp_err_t stop()
    {
        if (!initialized)
            return ESP_OK;
        esp_err_t result = ble ? bluetooth_control_set_enabled(false) : ESP_OK;
        if (result == ESP_OK)
            result = radio->deinitialize();
        if (auto *status = factory::services::context().connectivity_result)
            *status = radio->isInitialized() ? ESP_OK : ESP_ERR_INVALID_STATE;
        return result;
    }

    esp_err_t restore()
    {
        if (!initialized)
            return ESP_OK;
        esp_err_t result = radio->isInitialized()
                               ? ESP_OK
                               : radio->initialize(connectivity::Esp32c5ConnectivityConfig{});
        const auto &context = factory::services::context();
        if (result == ESP_OK && wifi && !radio->status().wifi_started)
            result = context.wifi_config != nullptr && context.wifi_config_valid != nullptr &&
                             *context.wifi_config_valid
                         ? radio->startWifiStation(*context.wifi_config)
                         : ESP_ERR_INVALID_STATE;
        if (result == ESP_OK && ble)
            result = bluetooth_control_set_enabled(true);
        if (context.connectivity_result != nullptr)
            *context.connectivity_result = result;
        if (result != ESP_OK)
            ESP_LOGE(kTag, "restore ESP32-C5 failed: %s", esp_err_to_name(result));
        return result;
    }
};

void restoreDisplay(bsp::t_panel_p4_round::Board &board)
{
    if (board.isDisplayInitialized())
    {
        if (!board.isTouchInitialized())
            (void)board.initializeTouch();
        factory::ui::resumeLvglDisplay();
    }
}
}

extern "C" esp_err_t settings_control_read_power(char *buffer, size_t buffer_size)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (buffer == nullptr || buffer_size == 0U || !board->isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    std::uint8_t percent{};
    std::uint16_t voltage{}, vbus{};
    std::int16_t current{};
    pdl::charge::Axp517PowerStatus power_status{};
    pdl::charge::Axp517FaultStatus fault_status{};
    if (board->readBatteryPercentage(percent) != ESP_OK ||
        board->readBatteryVoltageMv(voltage) != ESP_OK ||
        board->readBatteryCurrentMa(current) != ESP_OK ||
        board->readVbusVoltageMv(vbus) != ESP_OK ||
        board->readPowerStatus(power_status) != ESP_OK ||
        board->readPowerFaults(fault_status) != ESP_OK)
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
    pdl::charge::Axp517ModuleEnableStatus modules{};
    const esp_err_t module_error = board->readPowerModuleEnableStatus(modules);
    char module_raw[12];
    std::snprintf(module_raw, sizeof(module_raw), "0x%02X",
                  static_cast<unsigned>(modules.raw_config));
    const unsigned battery_power_mw = static_cast<unsigned>(voltage) *
                                      static_cast<unsigned>(current < 0 ? -current : current) / 1000U;
    const int written = std::snprintf(buffer, buffer_size,
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
                  "FAULT\t%s\n"
                  "INPUT CURRENT LIMIT\t%umA\n"
                  "INPUT VOLTAGE LIMIT\t%umV\n"
                  "MIN SYSTEM VOLTAGE\t%umV\n"
                  "CHARGE VOLTAGE\t%umV\n"
                  "CHARGE CURRENT\t%umA\n"
                  "VBUS GOOD\t%s\n"
                  "BOOST ENABLE\t%s",
                  static_cast<unsigned>(percent), static_cast<unsigned>(voltage),
                  static_cast<int>(current), battery_power_mw,
                  static_cast<unsigned>(vbus), power_status.vbus_present ? "YES" : "NO",
                  power_status.battery_present ? "YES" : "NO", direction, charge_state,
                  power_status.thermal_regulation ? "YES" : "NO",
                  power_status.input_current_limited || power_status.input_voltage_limited ? "YES" : "NO",
                  fault_status.system_over_voltage || fault_status.battery_under_voltage ? "ACTIVE" : "NONE",
                  static_cast<unsigned>(kInputCurrentLimitMa),
                  static_cast<unsigned>(kInputVoltageLimitMv),
                  static_cast<unsigned>(kMinimumSystemVoltageMv),
                  static_cast<unsigned>(kChargeVoltageMv),
                  static_cast<unsigned>(kChargeCurrentMa),
                  power_status.vbus_good ? "YES" : "NO",
                  module_error == ESP_OK ? (modules.boost_enabled ? "ON" : "OFF") : "READ ERROR");
    if (written < 0 || static_cast<size_t>(written) >= buffer_size)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
}

extern "C" esp_err_t settings_control_get_charger_enabled(bool *enabled)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (enabled == nullptr)
        return ESP_ERR_INVALID_ARG;
    return board->getChargerEnabled(*enabled);
}

extern "C" esp_err_t settings_control_set_charger_enabled(bool enabled)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    return board->setChargerEnabled(enabled);
}

extern "C" esp_err_t settings_control_get_backlight_brightness(std::uint8_t *percent)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (percent == nullptr)
        return ESP_ERR_INVALID_ARG;
    if (!board->isDisplayInitialized())
        return ESP_ERR_INVALID_STATE;
    *percent = board->backlightBrightness();
    return ESP_OK;
}

extern "C" esp_err_t settings_control_set_backlight_brightness(std::uint8_t percent)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    return board->setBacklightBrightness(percent);
}

extern "C" esp_err_t settings_control_get_audio_volume(std::uint8_t *percent)
{
    if (percent == nullptr)
        return ESP_ERR_INVALID_ARG;
    auto *board = factory::services::board();
    if (board == nullptr || !board->isAudioInitialized())
        return ESP_ERR_INVALID_STATE;
    // Match the -60..0 dB range used by the round Music page.
    *percent = board->isAudioMuted() ? 0U : static_cast<std::uint8_t>(
        std::clamp(std::lround((board->audioVolumeDb() + 60.0F) / 0.6F), 0L, 100L));
    return ESP_OK;
}

extern "C" esp_err_t settings_control_set_audio_volume(std::uint8_t percent)
{
    if (percent > 100U)
        return ESP_ERR_INVALID_ARG;
    auto *board = factory::services::board();
    if (board == nullptr || !board->isAudioInitialized())
        return ESP_ERR_INVALID_STATE;
    const float previous = board->audioVolumeDb();
    esp_err_t result = board->setAudioVolumeDb(-60.0F + static_cast<float>(percent) * 0.6F);
    if (result == ESP_OK)
    {
        result = board->setAudioMuted(percent == 0U);
        if (result != ESP_OK)
            (void)board->setAudioVolumeDb(previous);
    }
    return result;
}

extern "C" bool settings_control_is_sd_msc_enabled(void)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return false;
    return board->isUsbMscInitialized();
}

extern "C" esp_err_t settings_control_read_sd_info(char *buffer, size_t buffer_size)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;
    const sdmmc_card_t *card = board->sdCard();
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
    const char *access = board->isUsbMscInitialized()
                             ? "USB MSC"
                             : (board->isSdCardMounted() ? "FAT MOUNT" : "RAW");
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
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
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
    passed = board->isDisplayInitialized() &&
             board->displayPanel() != nullptr &&
             board->frameBuffer() != nullptr &&
             board->frameBufferCount() > 0U;
    detail[0] = '\0';
    if (!passed)
        checkErrorDetail(detail, sizeof(detail), ESP_ERR_INVALID_STATE);
    if (!appendCheckLine(buffer, buffer_size, used, "DISPLAY", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    auto *touch = board->touchController();
    std::array<std::uint8_t, 3> touch_id{};
    pdl::Status touch_status = pdl::Status::not_initialized;
    if (touch != nullptr)
    {
        touch_status = touch->native().readProductId(touch_id);

    }
    passed = pdl::isOk(touch_status);
    if (passed)
        std::snprintf(detail, sizeof(detail), " (GT%.3s)", reinterpret_cast<const char *>(touch_id.data()));
    else
        std::snprintf(detail, sizeof(detail), " (%s)", pdl::statusName(touch_status));
    if (!appendCheckLine(buffer, buffer_size, used, "TOUCH", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    pdl::charge::Axp517PowerStatus power_status{};
    const esp_err_t axp_error = board->readPowerStatus(power_status);
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
    if (!board->isLoraInitialized())
        lora_error = board->initializeLora();
    if (lora_error == ESP_OK)
        lora_error = board->loraStandby();
    passed = lora_error == ESP_OK;
    if (passed)
        std::snprintf(detail, sizeof(detail), " (LR2021 READY)");
    else
        checkErrorDetail(detail, sizeof(detail), lora_error);
    if (!appendCheckLine(buffer, buffer_size, used, "LORA", passed, detail))
        return ESP_ERR_INVALID_SIZE;
    failed += passed ? 0U : 1U;

    const esp_codec_dev_handle_t codec = board->audioCodec();
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

    sdmmc_card_t *card = board->sdCard();
    // TinyUSB owns the raw device while MSC is active. Do not issue SD commands.
    const esp_err_t sd_error = card == nullptr ? ESP_ERR_INVALID_STATE
                                  : (board->isUsbMscInitialized() ? ESP_ERR_NOT_FINISHED
                                                                 : sdmmc_get_status(card));
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
    esp_err_t camera_error = board->isCameraInitialized()
                                 ? board->readCameraProductId(camera_id)
                                 : board->initializeCamera();
    if (camera_error == ESP_OK && !board->isCameraInitialized())
        camera_error = ESP_ERR_INVALID_STATE;
    if (camera_error == ESP_OK)
        camera_error = board->readCameraProductId(camera_id);
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

    auto *radio = factory::services::connectivity();
    connectivity::Esp32c5CoprocessorInfo coprocessor_info{};
    const esp_err_t c5_error = radio != nullptr && radio->isInitialized()
                                   ? radio->coprocessorInfo(coprocessor_info)
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
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    bsp::t_panel_p4_round::SdCardConfig sd_config{};
    // Refuse to transfer SD ownership while an audio worker is using files.
    if (enabled && (board->isMusicPlaying() ||
                    board->isMusicPaused() || record_audio_is_recording() ||
                    record_audio_is_playing()))
        return ESP_ERR_INVALID_STATE;

    if (enabled)
    {
        if (board->isUsbMscInitialized())
            return ESP_OK;

        /* USB MSC needs exclusive raw block-device access. The normal
         * application starts with the card mounted through FatFS, so release
         * that owner before handing the card to TinyUSB. */
        if (board->isSdCardMounted())
        {
            const esp_err_t unmount_result = board->deinitializeSdCard();
            if (unmount_result != ESP_OK)
                return unmount_result;
        }

        const esp_err_t msc_result = board->initializeUsbMsc();
        if (msc_result != ESP_OK)
        {
            /* Keep the application usable when TinyUSB or the USB cable is
             * unavailable: restore the normal mounted SD-card state. */
            const esp_err_t restore_result = board->initializeSdCard(sd_config);
            if (restore_result != ESP_OK)
                ESP_LOGE(kTag, "restore SD mount after MSC failure failed: %s",
                         esp_err_to_name(restore_result));
        }
        return msc_result;
    }

    const esp_err_t msc_result = board->deinitializeUsbMsc();
    if (msc_result != ESP_OK)
        return msc_result;
    if (!board->isSdCardMounted())
    {
        const esp_err_t result = board->deinitializeSdCard();
        if (result != ESP_OK)
            return result;
        return board->initializeSdCard(sd_config);
    }
    return ESP_OK;
}


extern "C" esp_err_t settings_control_enter_ship_mode(void)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    return board->enterShipMode();
}

extern "C" esp_err_t settings_control_enter_deep_sleep(void)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (!record_audio_stop() || !record_audio_stop_cached_playback())
        return ESP_ERR_INVALID_STATE;
    RadioPause radio;
    esp_err_t result = radio.stop();
    if (result != ESP_OK)
    {
        (void)radio.restore();
        return result;
    }
    factory::ui::prepareLvglPowerDown(true);
    result = board->enterDeepSleep();
    if (result != ESP_OK)
    {
        restoreDisplay(*board);
        (void)radio.restore();
    }
    return result;
}

extern "C" esp_err_t settings_control_enter_light_sleep(void)
{
    auto *board = factory::services::board();
    if (board == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (record_audio_is_recording() || record_audio_is_playing() ||
        board->isMusicPlaying() || board->isMusicPaused())
        return ESP_ERR_INVALID_STATE;
    factory::ui::prepareLvglPowerDown(true);
    const esp_err_t result = board->enterLightSleep();
    restoreDisplay(*board);
    return result;
}
