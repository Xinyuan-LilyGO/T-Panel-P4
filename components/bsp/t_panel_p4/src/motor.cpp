#include <bsp/api/board.hpp>

#if T_PANEL_P4_BSP_IS_RECT

#include <limits>

#include <esp_check.h>
#include <esp_log.h>

#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_motor";

    bool isValidTimeout(std::uint32_t timeout_ms) noexcept
    {
        return timeout_ms > 0U &&
               timeout_ms <= static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    }

    void waitForMotorWakeup()
    {
        // Allow at least 1 ms for EN/standby wakeup, even at a tick boundary.
        vTaskDelay(pdMS_TO_TICKS(1) + 1);
    }

    esp_err_t finishPlayback(bsp::Board &board, esp_err_t error)
    {
        if (error != ESP_OK)
        {
            const esp_err_t cleanup = board.stopMotor();
            if (cleanup != ESP_OK)
                ESP_LOGE(kTag, "stop after playback failure: %s", esp_err_to_name(cleanup));
        }
        return error;
    }
}

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    esp_err_t Board::initializeMotor()
    {
        return initializeMotor(MotorConfig{});
    }

    esp_err_t Board::initializeMotor(const MotorConfig &config)
    {
        if (isMotorInitialized())
            return ESP_OK;
        ESP_RETURN_ON_FALSE(config.i2c_frequency_hz > 0U && config.i2c_frequency_hz <= 400000U &&
                                config.driver.waveform_library <= 7U &&
                                (config.driver.actuator == pdl::motor::Drv2605Actuator::erm ||
                                 config.driver.actuator == pdl::motor::Drv2605Actuator::lra),
                            ESP_ERR_INVALID_ARG, kTag, "invalid motor configuration");
        ESP_RETURN_ON_ERROR(deinitializeMotor(), kTag, "release previous motor resources failed");
        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize motor power/I2C failed");

        // Preload EN low before making it an output; R125 also pulls it low.
        ESP_RETURN_ON_ERROR(gpio_set_level(pins::motorEnable, 0), kTag, "preload motor EN failed");
        motor_enable_configured_ = true;
        gpio_config_t motor_gpio{};
        motor_gpio.pin_bit_mask = std::uint64_t{1} << pins::motorEnable;
        motor_gpio.mode = GPIO_MODE_OUTPUT;
        motor_gpio.intr_type = GPIO_INTR_DISABLE;
        esp_err_t error = gpio_config(&motor_gpio);
        if (error == ESP_OK)
            error = gpio_set_level(pins::motorEnable, 1);
        if (error == ESP_OK)
        {
            waitForMotorWakeup();
            drv2605_transport_.emplace(i2c_bus_, pdl::motor::Drv2605::defaultI2cAddress,
                                      config.i2c_frequency_hz);
            drv2605_.emplace(*drv2605_transport_);
            error = detail::toEspError(drv2605_->initialize(config.driver));
        }
        if (error == ESP_OK)
            error = stopMotor();
        if (error != ESP_OK)
        {
            const esp_err_t cleanup = deinitializeMotor();
            if (cleanup != ESP_OK)
                ESP_LOGE(kTag, "motor initialization cleanup failed: %s", esp_err_to_name(cleanup));
            ESP_LOGE(kTag, "DRV2605 initialization failed: %s", esp_err_to_name(error));
            return error;
        }
        ESP_LOGI(kTag, "DRV2605 initialized at 0x%02X, EN=GPIO%d, standby",
                 static_cast<unsigned>(pdl::motor::Drv2605::defaultI2cAddress),
                 static_cast<int>(pins::motorEnable));
        return ESP_OK;
    }

    esp_err_t Board::deinitializeMotor()
    {
        esp_err_t first_error = ESP_OK;
        const auto remember = [&first_error](esp_err_t error)
        {
            if (first_error == ESP_OK)
                first_error = error;
        };
        if (drv2605_ && drv2605_->isInitialized())
        {
            remember(stopMotor());
            remember(detail::toEspError(drv2605_->deinitialize()));
        }
        drv2605_.reset();
        drv2605_transport_.reset();
        // Disable hardware even if I2C stop/standby failed. After pin release,
        // the board's external EN pull-down keeps the driver disabled.
        if (motor_enable_configured_)
        {
            remember(gpio_set_level(pins::motorEnable, 0));
            const esp_err_t error = gpio_reset_pin(pins::motorEnable);
            remember(error);
            if (error == ESP_OK)
                motor_enable_configured_ = false;
        }
        return first_error;
    }

    bool Board::isMotorInitialized() const noexcept
    {
        return motor_enable_configured_ && drv2605_ && drv2605_->isInitialized();
    }

    esp_err_t Board::stopMotor(std::uint32_t timeout_ms)
    {
        if (!isMotorInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        // GO=0 alone does not stop RTP output. Attempt every stop operation even
        // if an earlier transaction fails; standby is the final output gate.
        esp_err_t first_error = detail::toEspError(drv2605_->stop(timeout_ms));
        const esp_err_t rtp_error = detail::toEspError(drv2605_->setRealtimeValue(0, timeout_ms));
        const esp_err_t standby_error = detail::toEspError(drv2605_->setStandby(true, timeout_ms));
        if (first_error == ESP_OK)
            first_error = rtp_error;
        return first_error != ESP_OK ? first_error : standby_error;
    }

    esp_err_t Board::prepareMotorPlayback(std::uint32_t timeout_ms)
    {
        ESP_RETURN_ON_ERROR(stopMotor(timeout_ms), kTag, "stop previous motor output failed");
        ESP_RETURN_ON_ERROR(detail::toEspError(drv2605_->setStandby(false, timeout_ms)), kTag,
                            "wake motor from standby failed");
        waitForMotorWakeup();
        return ESP_OK;
    }

    esp_err_t Board::playMotorEffect(std::uint8_t effect_id, std::uint32_t timeout_ms)
    {
        if (!isMotorInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms) || effect_id < pdl::motor::Drv2605::minimumEffectId ||
            effect_id > pdl::motor::Drv2605::maximumEffectId)
            return ESP_ERR_INVALID_ARG;
        esp_err_t error = prepareMotorPlayback(timeout_ms);
        if (error == ESP_OK)
            error = detail::toEspError(drv2605_->playEffect(effect_id, timeout_ms));
        return finishPlayback(*this, error);
    }

    esp_err_t Board::playMotorSequence(const std::uint8_t *effects, std::size_t effect_count,
                                       std::uint32_t timeout_ms)
    {
        if (!isMotorInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms) || effects == nullptr || effect_count == 0U ||
            effect_count > pdl::motor::Drv2605::sequenceSlots)
            return ESP_ERR_INVALID_ARG;
        for (std::size_t index = 0; index < effect_count; ++index)
        {
            if (effects[index] < pdl::motor::Drv2605::minimumEffectId ||
                effects[index] > pdl::motor::Drv2605::maximumEffectId)
                return ESP_ERR_INVALID_ARG;
        }
        esp_err_t error = prepareMotorPlayback(timeout_ms);
        if (error == ESP_OK)
            error = detail::toEspError(drv2605_->setSequence(effects, effect_count, timeout_ms));
        if (error == ESP_OK)
            error = detail::toEspError(drv2605_->playSequence(timeout_ms));
        return finishPlayback(*this, error);
    }

    esp_err_t Board::setMotorRealtimeValue(std::uint8_t value, std::uint32_t timeout_ms)
    {
        if (!isMotorInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms) || value > pdl::motor::Drv2605::rtpMaximumForward)
            return ESP_ERR_INVALID_ARG;
        if (value == 0U)
            return stopMotor(timeout_ms);
        pdl::motor::Drv2605Mode mode{};
        esp_err_t error = detail::toEspError(drv2605_->readMode(mode, timeout_ms));
        // Updating active RTP must not interrupt output with a standby cycle.
        if (error == ESP_OK && mode != pdl::motor::Drv2605Mode::real_time_playback)
            error = prepareMotorPlayback(timeout_ms);
        if (error == ESP_OK)
            error = detail::toEspError(drv2605_->setRealtimeValue(value, timeout_ms));
        return finishPlayback(*this, error);
    }

    esp_err_t Board::readMotorPlaying(bool &playing, std::uint32_t timeout_ms) const
    {
        if (!isMotorInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        pdl::motor::Drv2605Mode mode{};
        ESP_RETURN_ON_ERROR(detail::toEspError(drv2605_->readMode(mode, timeout_ms)), kTag,
                            "read motor mode failed");
        if (mode == pdl::motor::Drv2605Mode::standby)
        {
            playing = false;
            return ESP_OK;
        }
        if (mode == pdl::motor::Drv2605Mode::real_time_playback)
        {
            std::uint8_t value{};
            const esp_err_t error = detail::toEspError(drv2605_->readRealtimeValue(value, timeout_ms));
            if (error == ESP_OK)
                playing = value != 0U;
            return error;
        }
        if (mode != pdl::motor::Drv2605Mode::internal_trigger)
            return ESP_ERR_NOT_SUPPORTED;
        return detail::toEspError(drv2605_->isPlaying(playing, timeout_ms));
    }

    esp_err_t Board::readMotorStatus(std::uint8_t &status, std::uint32_t timeout_ms) const
    {
        if (!isMotorInitialized())
            return ESP_ERR_INVALID_STATE;
        if (!isValidTimeout(timeout_ms))
            return ESP_ERR_INVALID_ARG;
        return detail::toEspError(drv2605_->readStatus(status, timeout_ms));
    }

    pdl::motor::Drv2605 *Board::drv2605() noexcept
    {
        return isMotorInitialized() ? &*drv2605_ : nullptr;
    }
}

#endif
