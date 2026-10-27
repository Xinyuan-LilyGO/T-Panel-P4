#include <bsp/t_panel_p4_rect.hpp>

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_touch";
}

namespace bsp::t_panel_p4_rect
{
    esp_err_t Board::initializeTouch()
    {
        return initializeTouch(TouchConfig{});
    }

    esp_err_t Board::initializeTouch(const TouchConfig &config)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (isTouchInitialized())
            return ESP_OK;

        if (config.perform_hardware_reset)
        {
            ESP_RETURN_ON_ERROR(
                resetTouchController(),
                kTag,
                "touch hardware reset failed");
        }

        gpio_config_t interrupt_config{};
        interrupt_config.pin_bit_mask =
            std::uint64_t{1} << pins::touchInterrupt;
        interrupt_config.mode = GPIO_MODE_INPUT;
        interrupt_config.pull_up_en = GPIO_PULLUP_DISABLE;
        interrupt_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        interrupt_config.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(
            gpio_config(&interrupt_config),
            kTag,
            "configure touch interrupt input failed");

        ESP_RETURN_ON_ERROR(
            detail::toEspError(i2c_bus_.reset()),
            kTag,
            "reset I2C controller before touch initialization failed");

    esp_err_t error = ESP_FAIL;
    constexpr int kInitializationAttempts = 3;
    for (int attempt = 0; attempt < kInitializationAttempts; ++attempt)
    {
        if (attempt > 0)
        {
            if (config.perform_hardware_reset)
            {
                ESP_RETURN_ON_ERROR(
                    resetTouchController(),
                    kTag,
                    "touch hardware reset failed during retry");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        touch_transport_ = std::make_unique<pdl::esp_idf::I2cDevice>(
            i2c_bus_,
            pdl::touch::Ili9882Touch::defaultI2cAddress,
            pdl::touch::Ili9882Touch::defaultI2cFrequencyHz);
        vTaskDelay(pdMS_TO_TICKS(100));
        touch_ = std::make_unique<pdl::touch::Ili9882Touch>(*touch_transport_);

        error = detail::toEspError(
            touch_->initialize(config.perform_software_reset));
        if (error == ESP_OK)
            break;

        touch_.reset();
        touch_transport_.reset();
    }

    if (error != ESP_OK)
    {
        gpio_reset_pin(pins::touchInterrupt);
        ESP_LOGE(kTag, "ILI9882 initialization failed after %d attempts: %s",
                 kInitializationAttempts, esp_err_to_name(error));
        return error;
    }

    ESP_LOGI(kTag, "ILI9882 touch initialized on GPIO%d", pins::touchInterrupt);
        return ESP_OK;
    }

    esp_err_t Board::deinitializeTouch()
    {
        esp_err_t first_error = deinitializeTouchInterrupt();
    const bool had_touch = touch_ != nullptr || touch_transport_ != nullptr;

        if (touch_ && touch_->isInitialized())
        {
            const esp_err_t error = detail::toEspError(touch_->deinitialize());
            if (first_error == ESP_OK)
                first_error = error;
        }
        touch_.reset();
        touch_transport_.reset();

        if (had_touch && initialized_)
        {
            const esp_err_t error = gpio_set_level(pins::touchReset, 0);
            if (first_error == ESP_OK)
                first_error = error;
        }
        if (had_touch)
        {
            const esp_err_t error = gpio_reset_pin(pins::touchInterrupt);
            if (first_error == ESP_OK)
                first_error = error;
        }
        return first_error;
    }

    bool Board::isTouchInitialized() const noexcept
    {
        return touch_ && touch_->isInitialized();
    }

    esp_err_t Board::initializeTouchInterrupt(
        pdl::esp_idf::GpioInterrupt::IsrHandler handler,
        void *argument,
        gpio_int_type_t trigger)
    {
        if (!isTouchInitialized())
            return ESP_ERR_INVALID_STATE;
        if (touch_interrupt_.isInitialized())
            return ESP_OK;

        pdl::esp_idf::GpioInterrupt::Config config{};
        config.gpio = pins::touchInterrupt;
        config.trigger = trigger;
        return detail::toEspError(
            touch_interrupt_.initialize(config, handler, argument));
    }

    esp_err_t Board::deinitializeTouchInterrupt()
    {
        if (!touch_interrupt_.isInitialized())
            return ESP_OK;
        return detail::toEspError(touch_interrupt_.deinitialize());
    }

    pdl::touch::Ili9882Touch *Board::touchController() noexcept
    {
        return isTouchInitialized() ? &*touch_ : nullptr;
    }
}
