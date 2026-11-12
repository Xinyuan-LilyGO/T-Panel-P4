#include <bsp/board.hpp>

#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_touch";

    bool isValidTrigger(gpio_int_type_t trigger)
    {
        // Edge triggers avoid an unhandled asserted level continuously firing
        // when the application chooses to poll instead of supplying a callback.
        return trigger == GPIO_INTR_NEGEDGE || trigger == GPIO_INTR_POSEDGE ||
               trigger == GPIO_INTR_ANYEDGE;
    }
}

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    esp_err_t Board::initializeTouch()
    {
        return initializeTouch(TouchConfig{});
    }

    esp_err_t Board::initializeTouch(const TouchConfig &config)
    {
        ESP_RETURN_ON_FALSE(TouchController::isValidI2cAddress(config.i2c_address),
                            ESP_ERR_INVALID_ARG, kTag, "unsupported touch I2C address");
        ESP_RETURN_ON_FALSE(config.i2c_frequency_hz > 0 && config.i2c_frequency_hz <= 400000,
                            ESP_ERR_INVALID_ARG, kTag, "touch I2C frequency must be 1..400000 Hz");
        ESP_RETURN_ON_FALSE(isValidTrigger(config.interrupt_trigger), ESP_ERR_INVALID_ARG,
                            kTag, "touch interrupt requires an edge trigger");
        if (isTouchInitialized())
            return ESP_OK;
        if (!isI2cBusInitialized())
            ESP_RETURN_ON_ERROR(initializeI2cBus(), kTag, "initialize touch I2C failed");
        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize device power failed");

        touch_config_ = config;

        if (config.perform_hardware_reset)
        {
            const esp_err_t reset_error = resetTouchControllerImpl(!T_PANEL_P4_BSP_IS_RECT, false);
            if (reset_error != ESP_OK)
            {
                (void)gpio_reset_pin(pins::touchInterrupt);
                ESP_LOGE(kTag, "Touch hardware reset failed: %s", esp_err_to_name(reset_error));
                return reset_error;
            }
        }

#if T_PANEL_P4_BSP_IS_RECT
        // Distinguish an absent controller from a rejected ICE debug command.
        const esp_err_t probe_error = i2c_master_probe(i2c_bus_.nativeHandle(), config.i2c_address, 100);
        if (probe_error != ESP_OK)
        {
            ESP_LOGE(kTag, "ILI9882 did not respond at 0x%02X after reset: %s",
                     static_cast<unsigned int>(config.i2c_address), esp_err_to_name(probe_error));
            return probe_error;
        }
#endif

        // Configure the input/ISR exactly once. User callbacks become active only
        // after the controller is ready. Polling uses the same input with no callback.
        pdl::esp_idf::GpioInterrupt::Config interrupt_config{};
        interrupt_config.gpio = pins::touchInterrupt;
        interrupt_config.trigger = config.interrupt_trigger;
        esp_err_t error = detail::toEspError(touch_interrupt_.initialize(
            interrupt_config, touchInterruptHandler, this));
        if (error != ESP_OK)
            return error;

        touch_transport_.emplace(i2c_bus_, touch_config_.i2c_address, config.i2c_frequency_hz);
        touch_.emplace(*touch_transport_);
        error = detail::toEspError(touch_->initialize(config.perform_software_reset));
        if (error != ESP_OK)
        {
            touch_.reset();
            touch_transport_.reset();
            (void)deinitializeTouchInterrupt();
            ESP_LOGE(kTag, "Touch initialization failed: %s", esp_err_to_name(error));
            return error;
        }

        touch_interrupt_argument_.store(config.interrupt_argument);
        touch_interrupt_handler_.store(config.interrupt_handler);
        ESP_LOGI(kTag, "Touch initialized at 0x%02X, %lu Hz, interrupt GPIO%d",
                 static_cast<unsigned int>(touch_config_.i2c_address),
                 static_cast<unsigned long>(config.i2c_frequency_hz), pins::touchInterrupt);
        return ESP_OK;
    }

    esp_err_t Board::deinitializeTouch()
    {
        const bool had_touch = touch_.has_value() || touch_transport_.has_value();
        esp_err_t first_error = deinitializeTouchInterrupt();
        if (touch_ && touch_->isInitialized())
        {
            const esp_err_t error = detail::toEspError(touch_->deinitialize());
            if (first_error == ESP_OK)
                first_error = error;
        }
        touch_.reset();
        touch_transport_.reset();

        if (had_touch
#if !T_PANEL_P4_BSP_IS_RECT
            && isIoExpanderInitialized()
#endif
        )
        {
            const esp_err_t error = setControlOutput(pins::touchReset, false, !T_PANEL_P4_BSP_IS_RECT);
            if (first_error == ESP_OK)
                first_error = error;
        }
        return first_error;
    }

    bool Board::isTouchInitialized() const noexcept
    {
        return touch_ && touch_->isInitialized();
    }

    TouchConfig Board::touchConfig() const noexcept
    {
        return isTouchInitialized() ? touch_config_ : TouchConfig{};
    }

    void Board::touchInterruptHandler(void *argument)
    {
        auto *board = static_cast<Board *>(argument);
        if (const auto handler = board->touch_interrupt_handler_.load())
            handler(board->touch_interrupt_argument_.load());
    }

    esp_err_t Board::initializeTouchInterrupt(pdl::esp_idf::GpioInterrupt::IsrHandler handler,
                                              void *argument, gpio_int_type_t trigger)
    {
        if (!isTouchInitialized())
            return ESP_ERR_INVALID_STATE;
        if (handler == nullptr || !isValidTrigger(trigger))
            return ESP_ERR_INVALID_ARG;
        if (touch_interrupt_handler_.load() != nullptr)
            return ESP_OK;

        if (touch_interrupt_.isInitialized())
        {
            ESP_RETURN_ON_ERROR(detail::toEspError(touch_interrupt_.disable()), kTag,
                                "disable touch interrupt failed");
            ESP_RETURN_ON_ERROR(detail::toEspError(touch_interrupt_.setTrigger(trigger)), kTag,
                                "set touch interrupt trigger failed");
        }
        else
        {
            pdl::esp_idf::GpioInterrupt::Config config{};
            config.gpio = pins::touchInterrupt;
            config.trigger = trigger;
            ESP_RETURN_ON_ERROR(detail::toEspError(touch_interrupt_.initialize(
                config, touchInterruptHandler, this)), kTag, "initialize touch interrupt failed");
        }
        touch_interrupt_argument_.store(argument);
        touch_interrupt_handler_.store(handler);
        const esp_err_t error = detail::toEspError(touch_interrupt_.enable());
        if (error != ESP_OK)
        {
            touch_interrupt_handler_.store(nullptr);
            touch_interrupt_argument_.store(nullptr);
            return error;
        }
        touch_config_.interrupt_handler = handler;
        touch_config_.interrupt_argument = argument;
        touch_config_.interrupt_trigger = trigger;
        return ESP_OK;
    }

    esp_err_t Board::deinitializeTouchInterrupt()
    {
        esp_err_t error = ESP_OK;
        if (touch_interrupt_.isInitialized())
            error = detail::toEspError(touch_interrupt_.deinitialize());
        touch_interrupt_handler_.store(nullptr);
        touch_interrupt_argument_.store(nullptr);
        touch_config_.interrupt_handler = nullptr;
        touch_config_.interrupt_argument = nullptr;
        return error;
    }

    TouchController *Board::touchController() noexcept
    {
        return isTouchInitialized() ? &*touch_ : nullptr;
    }
}
