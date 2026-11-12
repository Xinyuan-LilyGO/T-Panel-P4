#include <bsp/board.hpp>

#include <esp_attr.h>
#include <esp_check.h>
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
        return bsp::T_PANEL_P4_BSP_NAMESPACE::detail::toEspError(status);
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

    esp_err_t verifyPowerDownRegisters(pdl::I2cDevice &transport)
    {
        struct Check { std::uint8_t address; std::uint8_t clear_mask; };
        // MOD_EN0, COMMON_CFG, MOD_EN1, battery detection and ADC enable.
        // Keep charger/Buck policy; their enable bits do not imply conversion.
        constexpr Check checks[]{{0x0B, 0x1C}, {0x10, 0x40}, {0x19, 0x34},
                                 {0x68, 0x01}, {0x90, 0xFF}};
        for (const auto &check : checks)
        {
            std::uint8_t value{};
            ESP_RETURN_ON_ERROR(pdlError(transport.writeRead(&check.address, 1, &value, 1, 100),
                                          "read AXP517 shutdown register"),
                                kTag, "PMIC shutdown readback failed");
            ESP_LOGI(kTag, "AXP517 shutdown readback: REG%02X=0x%02X clear-mask=0x%02X",
                     static_cast<unsigned>(check.address), static_cast<unsigned>(value),
                     static_cast<unsigned>(check.clear_mask));
            ESP_RETURN_ON_FALSE((value & check.clear_mask) == 0U, ESP_ERR_INVALID_STATE,
                                kTag, "AXP517 shutdown configuration not applied");
        }
        return ESP_OK;
    }
}

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    esp_err_t Board::initializePowerController(const PowerConfig &config)
    {
        if (isPowerInitialized())
            return ESP_OK;
        if (!isI2cBusInitialized())
            ESP_RETURN_ON_ERROR(initializeI2cBus(), kTag, "initialize power I2C failed");
        if (axp517_ || axp517_transport_ || power_event_task_ != nullptr ||
            power_event_stopped_ != nullptr || power_interrupt_mutex_ != nullptr)
        {
            return ESP_ERR_INVALID_STATE;
        }

        power_config_ = config;
        axp517_transport_.emplace(i2c_bus_, pdl::charge::Axp517::defaultI2cAddress,
                                  kI2cFrequencyHz);
        axp517_.emplace(*axp517_transport_);

        esp_err_t error = pdlError(axp517_->initialize(), "initialize AXP517");
        if (error == ESP_OK)
            error = pdlError(axp517_->setInputVoltageLimitMv(config.input_voltage_limit_mv),
                             "configure AXP517 input voltage limit");
        if (error == ESP_OK)
            error = pdlError(axp517_->setInputCurrentLimitMa(config.input_current_limit_ma),
                             "configure AXP517 input current limit");
        if (error == ESP_OK)
            error = pdlError(axp517_->setMinimumSystemVoltageMv(config.minimum_system_voltage_mv),
                             "configure AXP517 minimum system voltage");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeVoltageMv(config.charge_voltage_mv),
                             "configure AXP517 charge voltage");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeCurrentMa(config.charge_current_ma),
                             "configure AXP517 charge current");
        if (error == ESP_OK)
            error = pdlError(axp517_->setPrechargeCurrentMa(config.precharge_current_ma),
                             "configure AXP517 precharge current");
        if (error == ESP_OK)
            error = pdlError(axp517_->setTrickleCurrentMa(config.trickle_current_ma),
                             "configure AXP517 trickle current");
        if (error == ESP_OK)
            error = pdlError(axp517_->setTerminationCurrentMa(config.termination_current_ma),
                             "configure AXP517 termination current");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeTerminationEnabled(config.enable_charge_termination),
                             "configure AXP517 charge termination");
        if (error == ESP_OK)
            error = pdlError(axp517_->setBoostEnabled(false),
                             "disable AXP517 boost");
        if (error == ESP_OK)
            error = pdlError(axp517_->configureDischarge(true, true, true, 3U),
                             "configure AXP517 discharge");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeLedOutputType(config.charge_led_output_type),
                             "configure AXP517 CHGLED output type");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeLedCfgState(config.charge_led_cfg_state),
                             "configure AXP517 CHGLED CFG state");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeLedMode(config.charge_led_mode),
                             "configure AXP517 CHGLED mode");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargeLedEnabled(config.enable_charge_led),
                             "configure AXP517 CHGLED enabled");
        if (error == ESP_OK)
        {
            pdl::charge::Axp517ChargeLedConfig led{};
            error = pdlError(axp517_->readChargeLedConfig(led), "read AXP517 CHGLED configuration");
            if (error == ESP_OK &&
                (led.enabled != config.enable_charge_led ||
                 led.output_type != config.charge_led_output_type ||
                 led.mode != config.charge_led_mode || led.cfg_state != config.charge_led_cfg_state))
            {
                ESP_LOGE(kTag, "AXP517 CHGLED configuration readback mismatch");
                error = ESP_ERR_INVALID_STATE;
            }
        }
        if (error == ESP_OK)
            error = pdlError(axp517_->setAdcChannels(config.enable_adc ? pdl::charge::Axp517::adcAll : 0U),
                             "configure AXP517 ADC");
        if (error == ESP_OK)
            error = pdlError(axp517_->setFuelGaugeLowFrequency(false),
                             "disable AXP517 low-frequency fuel gauge");
        if (error == ESP_OK)
            error = pdlError(axp517_->setFuelGaugeEnabled(config.enable_fuel_gauge),
                             "configure AXP517 fuel gauge");
        if (error == ESP_OK)
            error = pdlError(axp517_->setBatteryDetectionEnabled(config.enable_battery_detection),
                             "configure AXP517 battery detection");
        if (error == ESP_OK)
            error = pdlError(axp517_->setBuckEnabled(true),
                             "enable AXP517 buck");
        if (error == ESP_OK)
            error = pdlError(axp517_->setChargerEnabled(config.enable_charger),
                             "configure AXP517 charger");
        if (error == ESP_OK)
            error = pdlError(axp517_->setBc12DetectionEnabled(config.enable_bc12_detection),
                             "configure AXP517 BC1.2 detection");
        if (error == ESP_OK)
            error = pdlError(axp517_->setTypeCDetectionEnabled(config.enable_type_c_detection),
                             "configure AXP517 Type-C detection");
        if (error == ESP_OK)
            error = pdlError(axp517_->setInterruptsEnabled(0U),
                             "disable AXP517 interrupts");
        if (error == ESP_OK)
            error = pdlError(axp517_->clearInterrupts(),
                             "clear AXP517 interrupts");

        if (error == ESP_OK)
        {
            power_interrupt_mutex_ = xSemaphoreCreateMutex();
            if (power_interrupt_mutex_ == nullptr)
                error = ESP_ERR_NO_MEM;
        }
        if (error == ESP_OK)
            error = setPowerInterruptEnabled(config.enable_interrupt);
        if (error != ESP_OK)
        {
            (void)deinitializePower();
            return error;
        }

        ESP_LOGI(kTag, "AXP517 power management initialized, IRQ=%s, mask=0x%08lX",
                 config.enable_interrupt ? "enabled" : "disabled",
                 static_cast<unsigned long>(config.enable_interrupt ? config.interrupt_mask : 0U));
        return ESP_OK;
    }

    esp_err_t Board::deinitializePower()
    {
        // Teardown from this task's callback would delete the running task.
        if (power_event_task_ != nullptr && xTaskGetCurrentTaskHandle() == power_event_task_)
            return ESP_ERR_INVALID_STATE;
        esp_err_t first_error = ESP_OK;
        power_interrupt_enabled_.store(false);

        if (power_interrupt_.isInitialized())
        {
            preserveFirstError(toEspError(power_interrupt_.disable()), first_error);
            preserveFirstError(toEspError(power_interrupt_.deinitialize()), first_error);
        }

        stopPowerEventTask();

        if (axp517_ && axp517_->isInitialized())
        {
            preserveFirstError(
                pdlError(axp517_->setInterruptsEnabled(0U), "disable AXP517 interrupts"),
                first_error);
            preserveFirstError(pdlError(axp517_->setChargeLedEnabled(false),
                                        "disable AXP517 charge LED"), first_error);
            preserveFirstError(pdlError(axp517_->setBoostEnabled(false),
                                        "disable AXP517 boost"), first_error);
            preserveFirstError(pdlError(axp517_->configureDischarge(false, true, true, 3U),
                                        "disable forced VBUS discharge"), first_error);
            // Releasing the transport does not clear PMIC registers. Disable
            // measurement and detection blocks explicitly for every board.
            preserveFirstError(pdlError(axp517_->setAdcChannels(0U), "disable AXP517 ADC"),
                               first_error);
            preserveFirstError(pdlError(axp517_->setFuelGaugeLowFrequency(false),
                                        "disable AXP517 low-frequency fuel gauge"),
                               first_error);
            preserveFirstError(
                pdlError(axp517_->setFuelGaugeEnabled(false), "disable AXP517 fuel gauge"),
                first_error);
            preserveFirstError(pdlError(axp517_->setBatteryDetectionEnabled(false),
                                        "disable AXP517 battery detection"),
                               first_error);
            preserveFirstError(pdlError(axp517_->setBc12DetectionEnabled(false),
                                        "disable AXP517 BC1.2 detection"), first_error);
            preserveFirstError(pdlError(axp517_->setTypeCDetectionEnabled(false),
                                        "disable AXP517 Type-C detection"), first_error);
            preserveFirstError(verifyPowerDownRegisters(*axp517_transport_), first_error);
            preserveFirstError(pdlError(axp517_->deinitialize(), "deinitialize AXP517"),
                               first_error);
        }
        axp517_.reset();
        axp517_transport_.reset();

        if (power_event_stopped_ != nullptr)
        {
            vSemaphoreDelete(power_event_stopped_);
            power_event_stopped_ = nullptr;
        }
        if (power_interrupt_mutex_ != nullptr)
        {
            vSemaphoreDelete(power_interrupt_mutex_);
            power_interrupt_mutex_ = nullptr;
        }
        power_task_stop_.store(false);
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
            xSemaphoreTake(power_event_stopped_, pdMS_TO_TICKS(kPowerTaskStopTimeoutMs)) != pdTRUE)
        {
            vTaskDelete(power_event_task_);
        }
        power_event_task_ = nullptr;
    }

    bool Board::isPowerInitialized() const noexcept
    {
        return axp517_ && axp517_->isInitialized();
    }

    esp_err_t Board::setPowerInterruptEnabled(bool enabled)
    {
        if (!isPowerInitialized() || power_interrupt_mutex_ == nullptr)
            return ESP_ERR_INVALID_STATE;

        xSemaphoreTake(power_interrupt_mutex_, portMAX_DELAY);
        if (enabled && power_interrupt_enabled_.load())
        {
            xSemaphoreGive(power_interrupt_mutex_);
            return ESP_OK;
        }

        power_interrupt_enabled_.store(false);
        esp_err_t error = ESP_OK;
        if (power_interrupt_.isInitialized())
            error = toEspError(power_interrupt_.disable());
        // Always try to mask the chip, even if GPIO masking failed.
        preserveFirstError(pdlError(axp517_->setInterruptsEnabled(0U),
                                    "disable AXP517 interrupts"), error);

        if (enabled && error == ESP_OK)
        {
            // Keep these resources until deinitialization. This allows the
            // callback to disable/re-enable IRQs without stopping its own task.
            if (power_event_stopped_ == nullptr)
            {
                power_event_stopped_ = xSemaphoreCreateBinary();
                if (power_event_stopped_ == nullptr)
                    error = ESP_ERR_NO_MEM;
            }
            if (error == ESP_OK && power_event_task_ == nullptr)
            {
                power_task_stop_.store(false);
                if (xTaskCreate(&Board::powerEventTaskEntry, "axp517_events", kPowerTaskStackWords,
                                this, kPowerTaskPriority, &power_event_task_) != pdPASS)
                    error = ESP_ERR_NO_MEM;
            }
            if (error == ESP_OK && !power_interrupt_.isInitialized())
            {
                pdl::esp_idf::GpioInterrupt::Config interrupt_config{};
                interrupt_config.gpio = pins::axp517Interrupt;
                interrupt_config.trigger = GPIO_INTR_NEGEDGE;
                // AXP517 INT is an open-drain, active-low output.
                interrupt_config.enable_internal_pullup = true;
                error = toEspError(power_interrupt_.initialize(
                    interrupt_config, &Board::powerInterruptHandler, this));
                if (error == ESP_OK)
                    error = toEspError(power_interrupt_.disable());
            }
            if (error == ESP_OK)
                error = pdlError(axp517_->clearInterrupts(), "clear AXP517 interrupts");
            if (error == ESP_OK)
                error = pdlError(axp517_->setInterruptsEnabled(power_config_.interrupt_mask),
                                 "enable AXP517 interrupts");
            if (error == ESP_OK)
            {
                power_interrupt_enabled_.store(true);
                error = toEspError(power_interrupt_.enable());
                // Do not lose an event that pulled INT low before GPIO enable.
                bool interrupt_high = true;
                if (error == ESP_OK)
                    error = toEspError(power_interrupt_.readLevel(interrupt_high));
                if (error == ESP_OK && !interrupt_high)
                    xTaskNotifyGive(power_event_task_);
            }
        }
        if (error != ESP_OK)
        {
            power_interrupt_enabled_.store(false);
            if (power_interrupt_.isInitialized())
                (void)power_interrupt_.disable();
            (void)axp517_->setInterruptsEnabled(0U);
        }
        xSemaphoreGive(power_interrupt_mutex_);
        return error;
    }

    bool Board::isPowerInterruptEnabled() const noexcept
    {
        return power_interrupt_enabled_.load();
    }

    esp_err_t Board::setPowerEventCallback(PowerEventCallback callback, void *context)
    {
        power_event_context_.store(context);
        power_event_callback_.store(callback);
        return ESP_OK;
    }

    esp_err_t Board::readPowerStatus(pdl::charge::Axp517PowerStatus &status,
                                     std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readPowerStatus(status, timeout_ms), "read AXP517 power status");
    }

    esp_err_t Board::readPowerModuleEnableStatus(pdl::charge::Axp517ModuleEnableStatus &status,
                                                 std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readModuleEnableStatus(status, timeout_ms),
                        "read AXP517 module enable status");
    }

    esp_err_t Board::setChargerEnabled(bool enabled, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->setChargerEnabled(enabled, timeout_ms), "AXP517 setChargerEnabled");
    }

    esp_err_t Board::getChargerEnabled(bool &enabled, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readChargerEnabled(enabled, timeout_ms), "AXP517 getChargerEnabled");
    }

    esp_err_t Board::setChargeLedEnabled(bool enabled, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->setChargeLedEnabled(enabled, timeout_ms), "AXP517 setChargeLedEnabled");
    }

    esp_err_t Board::setChargeLedOutputType(pdl::charge::Axp517ChargeLedOutputType type,
                                            std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->setChargeLedOutputType(type, timeout_ms), "AXP517 setChargeLedOutputType");
    }

    esp_err_t Board::setChargeLedMode(pdl::charge::Axp517ChargeLedMode mode,
                                      std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->setChargeLedMode(mode, timeout_ms), "AXP517 setChargeLedMode");
    }

    esp_err_t Board::setChargeLedCfgState(pdl::charge::Axp517ChargeLedCfgState state,
                                          std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->setChargeLedCfgState(state, timeout_ms), "AXP517 setChargeLedCfgState");
    }

    esp_err_t Board::readChargeLedConfig(pdl::charge::Axp517ChargeLedConfig &config,
                                         std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readChargeLedConfig(config, timeout_ms), "AXP517 readChargeLedConfig");
    }

    esp_err_t Board::readPowerFaults(pdl::charge::Axp517FaultStatus &status,
                                     std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readFaultStatus(status, timeout_ms), "read AXP517 fault status");
    }

    esp_err_t Board::clearPowerFaults(std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->clearFaults(0x0CU, timeout_ms), "clear AXP517 faults");
    }

    esp_err_t Board::readBatteryPercentage(std::uint8_t &percent, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readBatteryPercentage(percent, timeout_ms),
                        "read battery percentage");
    }

    esp_err_t Board::readBatteryVoltageMv(std::uint16_t &millivolts, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readBatteryVoltageMv(millivolts, timeout_ms),
                        "read battery voltage");
    }

    esp_err_t Board::readBatteryCurrentMa(std::int16_t &milliamps, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readBatteryCurrentMa(milliamps, timeout_ms),
                        "read battery current");
    }

    esp_err_t Board::readVbusVoltageMv(std::uint16_t &millivolts, std::uint32_t timeout_ms)
    {
        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;
        return pdlError(axp517_->readVbusVoltageMv(millivolts, timeout_ms), "read VBUS voltage");
    }

    void IRAM_ATTR Board::powerInterruptHandler(void *argument)
    {
        auto *board = static_cast<Board *>(argument);
        if (board == nullptr || !board->power_interrupt_enabled_.load() ||
            board->power_event_task_ == nullptr)
            return;

        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(board->power_event_task_, &higher_priority_task_woken);
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
            if (!board->power_interrupt_enabled_.load())
                continue;

            // INT is active-low and held until status is cleared. Mask the
            // GPIO while processing I2C, serialized with the runtime switch.
            xSemaphoreTake(board->power_interrupt_mutex_, portMAX_DELAY);
            if (board->power_interrupt_.isInitialized())
                (void)board->power_interrupt_.disable();
            xSemaphoreGive(board->power_interrupt_mutex_);

            PowerEvent event{};
            std::uint32_t interrupt_status{};
            esp_err_t error = pdlError(board->axp517_->readInterruptStatus(interrupt_status),
                                       "read AXP517 interrupt status");
            if (error == ESP_OK)
            {
                event.interrupt_status = interrupt_status;
                error = pdlError(board->axp517_->clearInterrupts(interrupt_status),
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
            event.type = error == ESP_OK ? PowerEventType::interrupt : PowerEventType::error;

            const PowerEventCallback callback = board->power_event_callback_.load();
            if (callback != nullptr)
                callback(event, board->power_event_context_.load());

            // The switch and rearm share a mutex, so disabling cannot be
            // undone by a task that was already processing an interrupt.
            vTaskDelay(pdMS_TO_TICKS(kPowerIrqRearmDelayMs));
            xSemaphoreTake(board->power_interrupt_mutex_, portMAX_DELAY);
            if (board->power_interrupt_enabled_.load() && !board->power_task_stop_.load() &&
                board->power_interrupt_.isInitialized())
            {
                bool interrupt_high = false;
                (void)board->power_interrupt_.readLevel(interrupt_high);
                if (!interrupt_high)
                    ESP_LOGW(kTag, "AXP517 GPIO%d remains low after IRQ handling",
                             static_cast<int>(pins::axp517Interrupt));
                (void)board->power_interrupt_.enable();
            }
            xSemaphoreGive(board->power_interrupt_mutex_);
        }

        if (board->power_event_stopped_ != nullptr)
            xSemaphoreGive(board->power_event_stopped_);
        vTaskDelete(nullptr);
    }
}
