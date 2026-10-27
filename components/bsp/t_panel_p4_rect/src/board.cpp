#include <bsp/t_panel_p4_rect.hpp>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <pdl/drivers/charge/axp517/axp517.hpp>

#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_rect";
    constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
    constexpr std::uint32_t kDisplaySleepDelayMilliseconds = 120;
    constexpr gpio_num_t kDeepSleepHeldOutputs[]{
        bsp::t_panel_p4_rect::pins::lcdBacklightPwm,
        bsp::t_panel_p4_rect::pins::motorEnable,
        bsp::t_panel_p4_rect::pins::i2sDataOut,
        bsp::t_panel_p4_rect::pins::i2sWordSelect,
        bsp::t_panel_p4_rect::pins::i2sBitClock,
        bsp::t_panel_p4_rect::pins::i2sMasterClock,
    };

    void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
    {
        if (first_error == ESP_OK && candidate != ESP_OK)
            first_error = candidate;
    }

    void releaseDeepSleepGpioHolds()
    {
        for (const gpio_num_t pin : kDeepSleepHeldOutputs)
            (void)gpio_hold_dis(pin);
    }

    esp_err_t holdDeepSleepOutputsLow()
    {
        for (const gpio_num_t pin : kDeepSleepHeldOutputs)
        {
            ESP_RETURN_ON_ERROR(
                gpio_set_level(pin, 0),
                kTag,
                "preload deep-sleep GPIO%d low failed",
                static_cast<int>(pin));
            ESP_RETURN_ON_ERROR(
                gpio_set_direction(pin, GPIO_MODE_OUTPUT),
                kTag,
                "configure deep-sleep GPIO%d output failed",
                static_cast<int>(pin));
            ESP_RETURN_ON_ERROR(
                gpio_set_level(pin, 0),
                kTag,
                "drive deep-sleep GPIO%d low failed",
                static_cast<int>(pin));
            ESP_RETURN_ON_ERROR(
                gpio_hold_en(pin),
                kTag,
                "hold deep-sleep GPIO%d low failed",
                static_cast<int>(pin));
        }
        return ESP_OK;
    }

    void disableLightSleepWakeup(
        const bsp::t_panel_p4_rect::LightSleepConfig &config)
    {
        if (config.timer_wakeup_us != 0U)
            (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        if (config.wakeup_gpio != GPIO_NUM_NC)
        {
            (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
            (void)gpio_wakeup_disable(config.wakeup_gpio);
        }
    }

    esp_err_t configureLightSleepWakeup(
        const bsp::t_panel_p4_rect::LightSleepConfig &config)
    {
        if (config.wakeup_gpio == GPIO_NUM_NC && config.timer_wakeup_us == 0U)
            return ESP_ERR_INVALID_ARG;

        if (config.wakeup_gpio != GPIO_NUM_NC)
        {
            if (!GPIO_IS_VALID_GPIO(config.wakeup_gpio))
                return ESP_ERR_INVALID_ARG;

            ESP_RETURN_ON_ERROR(
                gpio_set_direction(config.wakeup_gpio, GPIO_MODE_INPUT),
                kTag,
                "configure light-sleep wakeup GPIO failed");
            if (config.wakeup_on_high)
            {
                ESP_RETURN_ON_ERROR(
                    gpio_pullup_dis(config.wakeup_gpio),
                    kTag,
                    "disable wakeup GPIO pull-up failed");
                ESP_RETURN_ON_ERROR(
                    gpio_pulldown_en(config.wakeup_gpio),
                    kTag,
                    "enable wakeup GPIO pull-down failed");
            }
            else
            {
                ESP_RETURN_ON_ERROR(
                    gpio_pulldown_dis(config.wakeup_gpio),
                    kTag,
                    "disable wakeup GPIO pull-down failed");
                ESP_RETURN_ON_ERROR(
                    gpio_pullup_en(config.wakeup_gpio),
                    kTag,
                    "enable wakeup GPIO pull-up failed");
            }

            const int active_level = config.wakeup_on_high ? 1 : 0;
            if (gpio_get_level(config.wakeup_gpio) == active_level)
                return ESP_ERR_INVALID_STATE;

            ESP_RETURN_ON_ERROR(
                gpio_wakeup_enable(
                    config.wakeup_gpio,
                    config.wakeup_on_high ? GPIO_INTR_HIGH_LEVEL
                                          : GPIO_INTR_LOW_LEVEL),
                kTag,
                "enable light-sleep GPIO wakeup failed");
            const esp_err_t error = esp_sleep_enable_gpio_wakeup();
            if (error != ESP_OK)
            {
                (void)gpio_wakeup_disable(config.wakeup_gpio);
                return error;
            }
        }

        if (config.timer_wakeup_us != 0U)
        {
            const esp_err_t error =
                esp_sleep_enable_timer_wakeup(config.timer_wakeup_us);
            if (error != ESP_OK)
            {
                disableLightSleepWakeup(config);
                return error;
            }
        }
        return ESP_OK;
    }

    void disableDeepSleepWakeup(
        const bsp::t_panel_p4_rect::DeepSleepConfig &config)
    {
        if (config.timer_wakeup_us != 0U)
            (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        if (config.wakeup_gpio != GPIO_NUM_NC)
            (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    }

    esp_err_t configureDeepSleepWakeup(
        const bsp::t_panel_p4_rect::DeepSleepConfig &config)
    {
        if (config.wakeup_gpio != GPIO_NUM_NC)
        {
            if (!esp_sleep_is_valid_wakeup_gpio(config.wakeup_gpio))
                return ESP_ERR_INVALID_ARG;

            ESP_RETURN_ON_ERROR(
                gpio_set_direction(config.wakeup_gpio, GPIO_MODE_INPUT),
                kTag,
                "configure deep-sleep wakeup GPIO failed");
            if (config.wakeup_on_high)
            {
                ESP_RETURN_ON_ERROR(
                    gpio_pullup_dis(config.wakeup_gpio),
                    kTag,
                    "disable deep-sleep wakeup pull-up failed");
                ESP_RETURN_ON_ERROR(
                    gpio_pulldown_en(config.wakeup_gpio),
                    kTag,
                    "enable deep-sleep wakeup pull-down failed");
            }
            else
            {
                ESP_RETURN_ON_ERROR(
                    gpio_pulldown_dis(config.wakeup_gpio),
                    kTag,
                    "disable deep-sleep wakeup pull-down failed");
                ESP_RETURN_ON_ERROR(
                    gpio_pullup_en(config.wakeup_gpio),
                    kTag,
                    "enable deep-sleep wakeup pull-up failed");
            }

            const int active_level = config.wakeup_on_high ? 1 : 0;
            if (gpio_get_level(config.wakeup_gpio) == active_level)
                return ESP_ERR_INVALID_STATE;

            ESP_RETURN_ON_ERROR(
                esp_sleep_enable_ext1_wakeup_io(
                    std::uint64_t{1} << config.wakeup_gpio,
                    config.wakeup_on_high ? ESP_EXT1_WAKEUP_ANY_HIGH
                                          : ESP_EXT1_WAKEUP_ANY_LOW),
                kTag,
                "enable deep-sleep GPIO wakeup failed");
        }

        if (config.timer_wakeup_us != 0U)
        {
            const esp_err_t error =
                esp_sleep_enable_timer_wakeup(config.timer_wakeup_us);
            if (error != ESP_OK)
            {
                disableDeepSleepWakeup(config);
                return error;
            }
        }
        return ESP_OK;
    }
}

namespace bsp::t_panel_p4_rect::detail
{
    esp_err_t toEspError(pdl::Status status) noexcept
    {
        switch (status)
        {
        case pdl::Status::ok:
            return ESP_OK;
        case pdl::Status::invalid_argument:
            return ESP_ERR_INVALID_ARG;
        case pdl::Status::not_initialized:
        case pdl::Status::already_initialized:
        case pdl::Status::busy:
            return ESP_ERR_INVALID_STATE;
        case pdl::Status::device_not_found:
            return ESP_ERR_NOT_FOUND;
        case pdl::Status::timeout:
            return ESP_ERR_TIMEOUT;
        case pdl::Status::no_memory:
            return ESP_ERR_NO_MEM;
        case pdl::Status::unsupported:
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_FAIL;
        }
    }
}

namespace bsp::t_panel_p4_rect
{
    esp_err_t Board::initialize()
    {
        if (initialized_)
            return ESP_OK;

        releaseDeepSleepGpioHolds();

        esp_err_t error = initializeBacklight();
        if (error != ESP_OK)
        {
            (void)deinitialize();
            return error;
        }

        pdl::esp_idf::I2cBus::Config bus_config{};
        bus_config.sda = pins::i2cSda;
        bus_config.scl = pins::i2cScl;
        bus_config.enable_internal_pullup = true;
        error = detail::toEspError(i2c_bus_.initialize(bus_config));
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "I2C initialization failed: %s", esp_err_to_name(error));
            (void)deinitialize();
            return error;
        }

        initialized_ = true;
        error = initializePower();
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "AXP517 power initialization failed: %s", esp_err_to_name(error));
            (void)deinitialize();
            return error;
        }
        ESP_LOGI(kTag, "T-Panel-P4 rect BSP initialized");
        return ESP_OK;
    }

    esp_err_t Board::deinitialize()
    {
        esp_err_t first_error = prepareForPowerDown();
        // Do not tear down the shared I2C resources while an audio
        // callback could still reference the codec or I2S channels.
        if (first_error != ESP_OK &&
            (isCameraInitialized() || audio_initialized_ ||
             audio_tx_ != nullptr || audio_rx_ != nullptr))
        {
            return first_error;
        }
        if (backlight_initialized_)
        {
            preserveFirstError(setBacklightBrightness(0), first_error);
            preserveFirstError(
                ledc_stop(kBacklightSpeedMode, kBacklightChannel, 0),
                first_error);
            preserveFirstError(gpio_reset_pin(pins::lcdBacklightPwm), first_error);
            backlight_initialized_ = false;
            backlight_brightness_ = 0;
        }

        preserveFirstError(deinitializePower(), first_error);

        if (i2c_bus_.isInitialized())
            preserveFirstError(detail::toEspError(i2c_bus_.deinitialize()), first_error);

        initialized_ = false;
        speaker_enabled_ = false;
        display_power_enabled_ = false;
        return first_error;
    }

    esp_err_t Board::prepareForPowerDown()
    {
        esp_err_t first_error = deinitializeUsbMsc();
        if (isUsbMscInitialized())
            return first_error;
        preserveFirstError(deinitializeSdCard(), first_error);
        preserveFirstError(deinitializeCamera(), first_error);
        preserveFirstError(deinitializeDisplay(), first_error);
        preserveFirstError(deinitializeTouch(), first_error);
        preserveFirstError(deinitializeAudio(), first_error);
        return first_error;
    }

    esp_err_t Board::enterShipMode(std::uint32_t timeout_ms)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (timeout_ms == 0U)
            return ESP_ERR_INVALID_ARG;

        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;

        esp_err_t error = prepareForPowerDown();
        if (error != ESP_OK)
            return error;

        if (power_interrupt_.isInitialized())
        {
            preserveFirstError(
                detail::toEspError(power_interrupt_.disable()),
                error);
            preserveFirstError(
                detail::toEspError(power_interrupt_.deinitialize()),
                error);
        }
        stopPowerEventTask();
        if (error != ESP_OK)
            return error;

        ESP_LOGW(kTag, "Entering AXP517 ship mode");
        error = detail::toEspError(axp517_->enterShipMode(timeout_ms));
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "enter ship mode failed: %s", esp_err_to_name(error));
            const PowerConfig power_config = power_config_;
            (void)deinitializePower();
            const esp_err_t restore_error = initializePower(power_config);
            if (restore_error != ESP_OK)
            {
                ESP_LOGE(
                    kTag,
                    "AXP517 power service restore failed: %s",
                    esp_err_to_name(restore_error));
            }
            return error;
        }

        power_ship_mode_ = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        return deinitialize();
    }

    esp_err_t Board::enterLightSleep()
    {
        return enterLightSleep(LightSleepConfig{});
    }

    esp_err_t Board::enterLightSleep(const LightSleepConfig &config)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (isCameraInitialized() || isAudioInitialized() ||
            isSdCardInitialized())
        {
            ESP_LOGE(
                kTag,
                "stop camera, audio, and SD before light sleep");
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t error = configureLightSleepWakeup(config);
        if (error != ESP_OK)
            return error;

        const std::uint8_t saved_brightness = backlightBrightness();
        bool display_suspended = false;
        if (config.suspend_display && isDisplayInitialized())
        {
            error = setBacklightBrightness(0);
            if (error == ESP_OK)
                error = esp_lcd_panel_disp_on_off(panel_, false);
            if (error == ESP_OK)
                error = esp_lcd_panel_disp_sleep(panel_, true);
            if (error != ESP_OK)
            {
                (void)esp_lcd_panel_disp_on_off(panel_, true);
                (void)setBacklightBrightness(saved_brightness);
                disableLightSleepWakeup(config);
                return error;
            }
            display_suspended = true;
            vTaskDelay(pdMS_TO_TICKS(kDisplaySleepDelayMilliseconds));
        }

        ESP_LOGI(
            kTag,
            "Entering light sleep: GPIO=%d active-%s, timer=%llu us",
            static_cast<int>(config.wakeup_gpio),
            config.wakeup_on_high ? "high" : "low",
            static_cast<unsigned long long>(config.timer_wakeup_us));
        esp_err_t first_error = esp_light_sleep_start();

        if (display_suspended)
        {
            preserveFirstError(esp_lcd_panel_disp_sleep(panel_, false), first_error);
            vTaskDelay(pdMS_TO_TICKS(kDisplaySleepDelayMilliseconds));
            preserveFirstError(esp_lcd_panel_disp_on_off(panel_, true), first_error);
            preserveFirstError(setBacklightBrightness(saved_brightness), first_error);
        }
        disableLightSleepWakeup(config);
        ESP_LOGI(
            kTag,
            "Light sleep finished, wakeup cause=%d",
            static_cast<int>(esp_sleep_get_wakeup_cause()));
        return first_error;
    }

    esp_err_t Board::enterDeepSleep()
    {
        return enterDeepSleep(DeepSleepConfig{});
    }

    esp_err_t Board::enterDeepSleep(const DeepSleepConfig &config)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;

        esp_err_t error = configureDeepSleepWakeup(config);
        if (error != ESP_OK)
            return error;
        if (config.wakeup_gpio == GPIO_NUM_NC && config.timer_wakeup_us == 0U)
        {
            ESP_LOGW(
                kTag,
                "Deep sleep has no wakeup source; reset or power cycle is required");
        }

        error = prepareForPowerDown();
        if (error == ESP_OK)
            error = deinitialize();
        if (error == ESP_OK)
            error = holdDeepSleepOutputsLow();
        if (error != ESP_OK)
        {
            disableDeepSleepWakeup(config);
            return error;
        }

        ESP_LOGI(
            kTag,
            "Entering deep sleep: GPIO=%d active-%s, timer=%llu us",
            static_cast<int>(config.wakeup_gpio),
            config.wakeup_on_high ? "high" : "low",
            static_cast<unsigned long long>(config.timer_wakeup_us));
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_deep_sleep_start();
    }

    bool Board::isInitialized() const noexcept
    {
        return initialized_;
    }

    esp_err_t Board::setSpeakerEnabled(bool enabled)
    {
        ESP_RETURN_ON_ERROR(
            gpio_set_direction(pins::audioPowerEnable, GPIO_MODE_OUTPUT),
            kTag,
            "set speaker control failed");
        ESP_RETURN_ON_ERROR(
            gpio_set_level(pins::audioPowerEnable, enabled ? 1 : 0),
            kTag,
            "set speaker control level failed");

        ESP_LOGI(
            kTag,
            "speaker amplifier %s (GPIO%d)",
            enabled ? "enabled" : "disabled",
            static_cast<int>(pins::audioPowerEnable));
        speaker_enabled_ = enabled;
        return ESP_OK;
    }

    bool Board::isSpeakerEnabled() const noexcept
    {
        return speaker_enabled_;
    }

    esp_err_t Board::setSdPowerEnabled(bool enabled)
    {
        (void)enabled;
        if (!enabled && isSdCardInitialized())
            return ESP_ERR_INVALID_STATE;
        // RECT has no external SD power switch. The on-chip SD LDO is owned
        // by sd_card.cpp, so this hook is intentionally a no-op.
        return initialized_ ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_err_t Board::setDisplayPowerEnabled(bool enabled)
    {
        ESP_RETURN_ON_ERROR(
            gpio_set_direction(pins::lcdPower, GPIO_MODE_OUTPUT),
            kTag,
            "set display power control failed");
        ESP_RETURN_ON_ERROR(
            gpio_set_level(pins::lcdPower, enabled ? 1 : 0),
            kTag,
            "set display power control level failed");

        ESP_LOGI(
            kTag,
            "display power %s (GPIO%d)",
            enabled ? "enabled" : "disabled",
            static_cast<int>(pins::lcdPower));
        display_power_enabled_ = enabled;
        return ESP_OK;
    }

    bool Board::isDisplayPowerEnabled() const noexcept
    {
        return display_power_enabled_;
    }

    esp_err_t Board::resetTouchController()
    {
        ESP_RETURN_ON_ERROR(gpio_set_direction(pins::touchReset, GPIO_MODE_OUTPUT),
                            kTag, "configure touch reset failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(pins::touchReset, 1), kTag,
                            "set touch reset high failed");
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_RETURN_ON_ERROR(gpio_set_level(pins::touchReset, 0), kTag,
                            "assert touch reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_RETURN_ON_ERROR(gpio_set_level(pins::touchReset, 1), kTag,
                            "release touch reset failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        return ESP_OK;
    }

    pdl::esp_idf::I2cBus *Board::i2cBus() noexcept
    {
        return initialized_ ? &i2c_bus_ : nullptr;
    }

}
