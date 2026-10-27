#include <bsp/t_panel_p4_round.hpp>

#include <array>
#include <new>

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bsp_private.hpp"
#include "radiolib_esp_idf_hal.hpp"

namespace
{
constexpr char kTag[] = "t_panel_p4_lora";

std::array<LR2021PaTableEntry_t, RADIOLIB_LR2021_PA_TABLE_LEN>
    kLowFrequencyPaTable{{
        {1, 1, 8},
        {2, 2, 1},
        {2, 2, 3},
        {2, 2, 5},
        {1, 2, 13},
        {2, 1, 13},
        {2, 2, 11},
        {2, 2, 13},
        {3, 1, 12},
        {1, 1, 18},
        {1, 1, 20},
        {1, 1, 23},
        {1, 1, 27},
        {1, 1, 33},
        {1, 2, 26},
        {1, 2, 31},
        {1, 3, 27},
        {1, 1, 37},
        {1, 2, 40},
        {2, 1, 32},
        {2, 2, 32},
        {5, 1, 30},
        {4, 3, 31},
        {4, 2, 34},
        {5, 4, 33},
        {4, 4, 36},
        {5, 6, 36},
        {5, 6, 38},
        {6, 6, 39},
        {6, 6, 41},
        {7, 7, 42},
        {7, 6, 44},
    }};

const std::uint32_t kRfSwitchPins[Module::RFSWITCH_MAX_PINS] = {
    RADIOLIB_NC,
    RADIOLIB_LR2021_DIO6,
    RADIOLIB_LR2021_DIO7,
    RADIOLIB_NC,
    RADIOLIB_NC,
};

const Module::RfSwitchMode_t kRfSwitchTable[] = {
    {LR2021::MODE_STBY, {0, 0, 0}},
    {LR2021::MODE_RX, {0, 0, 0}},
    {LR2021::MODE_TX, {0, 0, 0}},
    {LR2021::MODE_RX_HF, {0, 1, 0}},
    {LR2021::MODE_TX_HF, {0, 0, 1}},
    END_OF_MODE_TABLE,
};

esp_err_t toEspError(std::int16_t status) noexcept
{
    switch (status)
    {
    case RADIOLIB_ERR_NONE:
        return ESP_OK;
    case RADIOLIB_ERR_CHIP_NOT_FOUND:
        return ESP_ERR_NOT_FOUND;
    case RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED:
        return ESP_ERR_NO_MEM;
    case RADIOLIB_ERR_TX_TIMEOUT:
    case RADIOLIB_ERR_RX_TIMEOUT:
    case RADIOLIB_ERR_SPI_CMD_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case RADIOLIB_ERR_UNSUPPORTED:
    case RADIOLIB_ERR_INVALID_FUNCTION:
        return ESP_ERR_NOT_SUPPORTED;
    case RADIOLIB_ERR_NULL_POINTER:
    case RADIOLIB_ERR_INVALID_BANDWIDTH:
    case RADIOLIB_ERR_INVALID_SPREADING_FACTOR:
    case RADIOLIB_ERR_INVALID_CODING_RATE:
    case RADIOLIB_ERR_INVALID_FREQUENCY:
    case RADIOLIB_ERR_INVALID_OUTPUT_POWER:
    case RADIOLIB_ERR_INVALID_PREAMBLE_LENGTH:
    case RADIOLIB_ERR_INVALID_SYNC_WORD:
    case RADIOLIB_ERR_INVALID_TCXO_VOLTAGE:
        return ESP_ERR_INVALID_ARG;
    default:
        return ESP_FAIL;
    }
}

void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
{
    if (first_error == ESP_OK && candidate != ESP_OK)
        first_error = candidate;
}

void delayMilliseconds(std::uint32_t milliseconds)
{
    if (milliseconds == 0U)
        return;
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (ticks == 0)
        ticks = 1;
    vTaskDelay(ticks);
}
}

namespace bsp::t_panel_p4_round
{
esp_err_t Board::initializeLora()
{
    return initializeLora(LoraConfig{});
}

esp_err_t Board::initializeLora(const LoraConfig &config)
{
    if (!initialized_)
        return ESP_ERR_INVALID_STATE;
    if (isLoraInitialized())
        return ESP_OK;
    if (config.spi_frequency_hz == 0U || config.frequency_mhz <= 0.0F ||
        config.bandwidth_khz <= 0.0F || config.preamble_length == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    lora_last_error_ = RADIOLIB_ERR_NONE;
    ESP_RETURN_ON_ERROR(
        setLoraPowerEnabled(true),
        kTag,
        "enable LR2021 power failed");
    delayMilliseconds(config.power_up_delay_ms);

    esp_err_t error = ESP_OK;

    if (!spi_bus_.isInitialized())
    {
        pdl::esp_idf::SpiBus::Config bus_config{};
        bus_config.mosi = pins::spiMosi;
        bus_config.miso = pins::spiMiso;
        bus_config.sclk = pins::spiSck;
        bus_config.host = SPI2_HOST;
        bus_config.dma_channel = SPI_DMA_CH_AUTO;
        error = detail::toEspError(spi_bus_.initialize(bus_config));
        if (error != ESP_OK)
        {
            ESP_LOGE(
                kTag,
                "SPI bus initialization failed: %s",
                esp_err_to_name(error));
            (void)deinitializeLora();
            return error;
        }
    }

    auto *hal = new (std::nothrow) detail::RadioLibEspIdfHal(
        spi_bus_,
        config.spi_frequency_hz);
    if (hal == nullptr)
    {
        (void)deinitializeLora();
        return ESP_ERR_NO_MEM;
    }
    lora_hal_.reset(hal);

    lora_module_.reset(new (std::nothrow) Module(
        lora_hal_.get(),
        pins::loraChipSelect,
        pins::loraInterrupt,
        pins::loraReset,
        pins::loraBusy));
    if (!lora_module_)
    {
        (void)deinitializeLora();
        return ESP_ERR_NO_MEM;
    }

    lora_radio_.reset(new (std::nothrow) LR2021(lora_module_.get()));
    if (!lora_radio_)
    {
        (void)deinitializeLora();
        return ESP_ERR_NO_MEM;
    }

    lora_radio_->irqDioNum = pins::loraIrqDio;
    const bool high_frequency =
        config.frequency_mhz > RADIOLIB_LR2021_LF_CUTOFF_FREQ;
    if (!high_frequency)
    {
        lora_radio_->setPaTable(
            kLowFrequencyPaTable.data(),
            false);
    }

    lora_last_error_ = lora_radio_->begin(
        config.frequency_mhz,
        config.bandwidth_khz,
        config.spreading_factor,
        config.coding_rate,
        config.sync_word,
        config.output_power_dbm,
        config.preamble_length,
        config.tcxo_voltage);
    if (lora_last_error_ == RADIOLIB_ERR_NONE && hal->lastError() != ESP_OK)
    {
        error = hal->lastError();
    }
    else
    {
        error = toEspError(lora_last_error_);
    }
    if (error != ESP_OK)
    {
        ESP_LOGE(
            kTag,
            "LR2021 begin failed: RadioLib=%d, HAL=%s",
            static_cast<int>(lora_last_error_),
            esp_err_to_name(hal->lastError()));
        (void)deinitializeLora();
        return error;
    }

    lora_radio_->setRfSwitchTable(kRfSwitchPins, kRfSwitchTable);
    lora_last_error_ = lora_radio_->setCRC(config.crc_enabled ? 2U : 0U);
    if (lora_last_error_ == RADIOLIB_ERR_NONE)
        lora_last_error_ = lora_radio_->invertIQ(config.invert_iq);
    if (lora_last_error_ != RADIOLIB_ERR_NONE)
    {
        error = toEspError(lora_last_error_);
        ESP_LOGE(
            kTag,
            "LR2021 packet configuration failed: %d",
            static_cast<int>(lora_last_error_));
        (void)deinitializeLora();
        return error;
    }

    lora_config_ = config;
    lora_initialized_ = true;
    ESP_LOGI(
        kTag,
        "LR2021 initialized: %.1f MHz, %.1f kHz, SF%u, CR4/%u, %d dBm, DIO%u -> GPIO%d",
        static_cast<double>(config.frequency_mhz),
        static_cast<double>(config.bandwidth_khz),
        static_cast<unsigned int>(config.spreading_factor),
        static_cast<unsigned int>(config.coding_rate),
        static_cast<int>(config.output_power_dbm),
        static_cast<unsigned int>(pins::loraIrqDio),
        static_cast<int>(pins::loraInterrupt));
    return ESP_OK;
}

esp_err_t Board::deinitializeLora()
{
    esp_err_t first_error = ESP_OK;
    if (lora_radio_ && lora_initialized_)
    {
        lora_radio_->clearPacketReceivedAction();
        lora_radio_->clearPacketSentAction();
        lora_last_error_ = lora_radio_->standby();
        preserveFirstError(toEspError(lora_last_error_), first_error);
    }

    lora_initialized_ = false;
    lora_radio_.reset();
    if (lora_module_)
        lora_module_->term();
    lora_module_.reset();
    lora_hal_.reset();
    lora_config_.reset();

    if (initialized_)
        preserveFirstError(
            setLoraPowerEnabled(false),
            first_error);

    preserveFirstError(gpio_reset_pin(pins::loraChipSelect), first_error);
    preserveFirstError(gpio_reset_pin(pins::loraBusy), first_error);
    preserveFirstError(gpio_reset_pin(pins::loraReset), first_error);
    preserveFirstError(gpio_reset_pin(pins::loraInterrupt), first_error);
    return first_error;
}

bool Board::isLoraInitialized() const noexcept
{
    return lora_initialized_ && lora_radio_ != nullptr;
}

esp_err_t Board::loraStandby()
{
    if (!isLoraInitialized())
        return ESP_ERR_INVALID_STATE;
    lora_last_error_ = lora_radio_->standby();
    return toEspError(lora_last_error_);
}

esp_err_t Board::loraSleep()
{
    if (!isLoraInitialized())
        return ESP_ERR_INVALID_STATE;
    lora_last_error_ = lora_radio_->sleep();
    return toEspError(lora_last_error_);
}

std::int16_t Board::loraLastError() const noexcept
{
    return lora_last_error_;
}

LR2021 *Board::loraRadio() noexcept
{
    return isLoraInitialized() ? lora_radio_.get() : nullptr;
}

const LoraConfig *Board::loraConfig() const noexcept
{
    return isLoraInitialized() && lora_config_ ? &*lora_config_ : nullptr;
}
}
