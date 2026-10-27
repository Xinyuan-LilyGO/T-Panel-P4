#include <bsp/t_panel_p4_standard.hpp>

#include <esp_attr.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bsp_private.hpp"

namespace
{
constexpr char kTag[] = "t_panel_p4_power";
constexpr std::uint32_t kI2cFrequencyHz = 400000;
constexpr std::uint32_t kPowerTaskStackWords = 4096;
constexpr UBaseType_t kPowerTaskPriority = 5;
constexpr std::uint32_t kPowerTaskStopTimeoutMs = 500;
constexpr std::uint32_t kPowerIrqRearmDelayMs = 5;

esp_err_t toEspError(pdl::Status status)
{
    return bsp::t_panel_p4_standard::detail::toEspError(status);
}

esp_err_t pdlError(pdl::Status status, const char *operation)
{
    if (pdl::isOk(status))
        return ESP_OK;
    ESP_LOGE(kTag, "%s failed: %s", operation, pdl::statusName(status));
    return toEspError(status);
}

void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
{
    if (first_error == ESP_OK && candidate != ESP_OK)
        first_error = candidate;
}
}

namespace bsp::t_panel_p4_standard
{
esp_err_t Board::initializePower()
{
    return initializePower(PowerConfig{});
}

esp_err_t Board::initializePower(const PowerConfig &config)
{
    if (!initialized_)
        return ESP_ERR_INVALID_STATE;
    if (isPowerInitialized())
        return ESP_OK;
    if (axp517_ || axp517_transport_ || power_event_task_ != nullptr ||
        power_event_stopped_ != nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    power_config_ = config;
    power_ship_mode_ = false;
    axp517_transport_.emplace(
        i2c_bus_,
        pdl::charge::Axp517::defaultI2cAddress,
        kI2cFrequencyHz);
    axp517_.emplace(*axp517_transport_);

    esp_err_t error = pdlError(axp517_->initialize(), "initialize AXP517");
    if (error != ESP_OK)
        return deinitializePower(), error;

    if (config.enable_adc)
    {
        error = pdlError(
            axp517_->setAdcChannels(pdl::charge::Axp517::adcAll),
            "enable AXP517 ADC");
    }
    if (error == ESP_OK && config.enable_fuel_gauge)
    {
        error = pdlError(
            axp517_->setFuelGaugeEnabled(true),
            "enable AXP517 fuel gauge");
    }
    if (error == ESP_OK && config.enable_battery_detection)
    {
        error = pdlError(
            axp517_->setBatteryDetectionEnabled(true),
            "enable AXP517 battery detection");
    }
    if (error == ESP_OK)
    {
        error = pdlError(
            axp517_->clearInterrupts(),
            "clear AXP517 interrupts");
    }
    if (error == ESP_OK)
    {
        error = pdlError(
            axp517_->setInterruptsEnabled(
                config.enable_interrupt ? config.interrupt_mask : 0U),
            "configure AXP517 interrupts");
    }
    if (error != ESP_OK)
        return deinitializePower(), error;

    if (config.enable_interrupt)
    {
        power_event_stopped_ = xSemaphoreCreateBinary();
        if (power_event_stopped_ == nullptr)
            return deinitializePower(), ESP_ERR_NO_MEM;

        power_task_stop_.store(false);
        if (xTaskCreate(
                &Board::powerEventTaskEntry,
                "axp517_events",
                kPowerTaskStackWords,
                this,
                kPowerTaskPriority,
                &power_event_task_) != pdPASS)
        {
            return deinitializePower(), ESP_ERR_NO_MEM;
        }
    }

    if (config.enable_interrupt)
    {
        pdl::esp_idf::GpioInterrupt::Config interrupt_config{};
        interrupt_config.gpio = pins::axp517Interrupt;
        interrupt_config.trigger = GPIO_INTR_NEGEDGE;
        // AXP517 INT is an open-drain, active-low output.  The board has an
        // external pull-up on normal hardware, but enabling the internal one
        // keeps the input deterministic during power-up and diagnostics.
        interrupt_config.enable_internal_pullup = true;
        error = toEspError(power_interrupt_.initialize(
            interrupt_config,
            &Board::powerInterruptHandler,
            this));
        if (error == ESP_OK)
            error = toEspError(power_interrupt_.enable());
        if (error != ESP_OK)
            return deinitializePower(), error;
    }

    ESP_LOGI(
        kTag,
        "AXP517 power management initialized, IRQ=%s, mask=0x%08lX",
        config.enable_interrupt ? "enabled" : "disabled",
        static_cast<unsigned long>(
            config.enable_interrupt ? config.interrupt_mask : 0U));
    return ESP_OK;
}

esp_err_t Board::deinitializePower()
{
    esp_err_t first_error = ESP_OK;

    if (power_interrupt_.isInitialized())
    {
        preserveFirstError(
            toEspError(power_interrupt_.disable()),
            first_error);
        preserveFirstError(
            toEspError(power_interrupt_.deinitialize()),
            first_error);
    }

    stopPowerEventTask();

    if (!power_ship_mode_ && axp517_ && axp517_->isInitialized())
    {
        preserveFirstError(
            pdlError(
                axp517_->setInterruptsEnabled(0U),
                "disable AXP517 interrupts"),
            first_error);
        preserveFirstError(
            pdlError(axp517_->deinitialize(), "deinitialize AXP517"),
            first_error);
    }
    axp517_.reset();
    axp517_transport_.reset();

    if (power_event_stopped_ != nullptr)
    {
        vSemaphoreDelete(power_event_stopped_);
        power_event_stopped_ = nullptr;
    }
    power_task_stop_.store(false);
    power_ship_mode_ = false;
    power_event_callback_.store(nullptr);
    power_event_context_.store(nullptr);
    return first_error;
}

void Board::stopPowerEventTask() noexcept
{
    if (power_event_task_ == nullptr)
        return;

    power_task_stop_.store(true);
    xTaskNotifyGive(power_event_task_);
    if (power_event_stopped_ == nullptr ||
        xSemaphoreTake(
            power_event_stopped_,
            pdMS_TO_TICKS(kPowerTaskStopTimeoutMs)) != pdTRUE)
    {
        vTaskDelete(power_event_task_);
    }
    power_event_task_ = nullptr;
}

bool Board::isPowerInitialized() const noexcept
{
    return axp517_ && axp517_->isInitialized();
}

esp_err_t Board::setPowerEventCallback(
    PowerEventCallback callback,
    void *context)
{
    power_event_context_.store(context);
    power_event_callback_.store(callback);
    return ESP_OK;
}

esp_err_t Board::readPowerStatus(
    pdl::charge::Axp517PowerStatus &status,
    std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->readPowerStatus(status, timeout_ms),
        "read AXP517 power status");
}

esp_err_t Board::readPowerFaults(
    pdl::charge::Axp517FaultStatus &status,
    std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->readFaultStatus(status, timeout_ms),
        "read AXP517 fault status");
}

esp_err_t Board::clearPowerFaults(std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->clearFaults(0x0CU, timeout_ms),
        "clear AXP517 faults");
}

esp_err_t Board::readBatteryPercentage(
    std::uint8_t &percent,
    std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->readBatteryPercentage(percent, timeout_ms),
        "read battery percentage");
}

esp_err_t Board::readBatteryVoltageMv(
    std::uint16_t &millivolts,
    std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->readBatteryVoltageMv(millivolts, timeout_ms),
        "read battery voltage");
}

esp_err_t Board::readBatteryCurrentMa(
    std::int16_t &milliamps,
    std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->readBatteryCurrentMa(milliamps, timeout_ms),
        "read battery current");
}

esp_err_t Board::readVbusVoltageMv(
    std::uint16_t &millivolts,
    std::uint32_t timeout_ms)
{
    if (!isPowerInitialized())
        return ESP_ERR_INVALID_STATE;
    return pdlError(
        axp517_->readVbusVoltageMv(millivolts, timeout_ms),
        "read VBUS voltage");
}

void IRAM_ATTR Board::powerInterruptHandler(void *argument)
{
    auto *board = static_cast<Board *>(argument);
    if (board == nullptr || board->power_event_task_ == nullptr)
        return;

    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(
        board->power_event_task_,
        &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE)
        portYIELD_FROM_ISR();
}

void Board::powerEventTaskEntry(void *argument)
{
    auto *board = static_cast<Board *>(argument);
    if (board == nullptr)
    {
        vTaskDelete(nullptr);
        return;
    }

    while (!board->power_task_stop_.load())
    {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == 0U)
            continue;
        if (board->power_task_stop_.load() || !board->isPowerInitialized())
            break;

        // GPIO4 is an active-low, level-held indication on AXP517.  Disable
        // the edge interrupt while the I2C transaction group is in progress;
        // otherwise a pending status bit can continuously wake this task and
        // starve other devices on the shared I2C bus (notably XL9555).
        if (board->power_interrupt_.isInitialized())
            (void)board->power_interrupt_.disable();

        PowerEvent event{};
        std::uint32_t interrupt_status{};
        esp_err_t error = pdlError(
            board->axp517_->readInterruptStatus(interrupt_status),
            "read AXP517 interrupt status");
        if (error == ESP_OK)
        {
            event.interrupt_status = interrupt_status;
            error = pdlError(
                board->axp517_->clearInterrupts(interrupt_status),
                "clear AXP517 interrupt status");
        }
        // A spurious edge can produce an empty status word.  It is still
        // reported to the application, but avoid four more I2C reads when
        // there is no AXP517 event to decode.
        if (error == ESP_OK && interrupt_status != 0U)
        {
            error = board->readPowerStatus(event.power_status);
        }
        if (error == ESP_OK && interrupt_status != 0U)
        {
            error = board->readPowerFaults(event.fault_status);
        }
        event.error = error;
        event.type = error == ESP_OK
                         ? PowerEventType::interrupt
                         : PowerEventType::error;

        const PowerEventCallback callback =
            board->power_event_callback_.load();
        if (callback != nullptr)
            callback(event, board->power_event_context_.load());

        // Give the shared I2C bus a scheduling window before re-arming.  The
        // interrupt is edge-triggered, so re-enabling it while INT is still
        // low does not retrigger the current event and avoids losing future
        // falling edges.
        if (board->power_interrupt_.isInitialized())
        {
            bool interrupt_high = false;
            (void)board->power_interrupt_.readLevel(interrupt_high);
            if (!interrupt_high)
            {
                ESP_LOGW(
                    kTag,
                    "AXP517 GPIO4 remains low after IRQ handling");
            }
            vTaskDelay(pdMS_TO_TICKS(kPowerIrqRearmDelayMs));
            (void)board->power_interrupt_.enable();
        }
    }

    if (board->power_event_stopped_ != nullptr)
        xSemaphoreGive(board->power_event_stopped_);
    vTaskDelete(nullptr);
}
}
