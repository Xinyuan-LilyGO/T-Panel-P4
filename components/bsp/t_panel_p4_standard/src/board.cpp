#include <bsp/t_panel_p4_standard.hpp>

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
    constexpr char kTag[] = "t_panel_p4_standard";
    constexpr std::uint32_t kI2cFrequencyHz = 400000;
    constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
    constexpr std::uint32_t kDisplaySleepDelayMilliseconds = 120;
    constexpr gpio_num_t kDeepSleepHeldOutputs[]{
        bsp::t_panel_p4_standard::pins::lcdBacklightPwm,
        bsp::t_panel_p4_standard::pins::esp32c5Enable,
        bsp::t_panel_p4_standard::pins::i2sDataOut,
        bsp::t_panel_p4_standard::pins::i2sWordSelect,
        bsp::t_panel_p4_standard::pins::i2sBitClock,
        bsp::t_panel_p4_standard::pins::i2sMasterClock,
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
        const bsp::t_panel_p4_standard::LightSleepConfig &config)
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
        const bsp::t_panel_p4_standard::LightSleepConfig &config)
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
            // 启用ESP32定时器唤醒功能，设置唤醒时间
            const esp_err_t error =                                    // 定义一个错误码变量，用于存储函数返回结果
                esp_sleep_enable_timer_wakeup(config.timer_wakeup_us); // 调用ESP32的API函数，设置定时器唤醒时间，参数为配置中的微秒数
            if (error != ESP_OK)
            {
                disableLightSleepWakeup(config);
                return error;
            }
        }
        return ESP_OK;
    }

    void disableDeepSleepWakeup(
        const bsp::t_panel_p4_standard::DeepSleepConfig &config)
    {
        if (config.timer_wakeup_us != 0U)
            (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
        if (config.wakeup_gpio != GPIO_NUM_NC)
            (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
    }

    esp_err_t configureDeepSleepWakeup(
        const bsp::t_panel_p4_standard::DeepSleepConfig &config)
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

namespace bsp::t_panel_p4_standard::detail
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

namespace bsp::t_panel_p4_standard
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

        xl9555_transport_.emplace(
            i2c_bus_,
            pins::xl9555Address,
            kI2cFrequencyHz);
        xl9555_.emplace(*xl9555_transport_);
        error = detail::toEspError(xl9555_->initialize());
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "XL9555 initialization failed: %s", esp_err_to_name(error));
            (void)deinitialize();
            return error;
        }

        initialized_ = true;

        setC5AudioLcdEnabled(true);
        error = initializePower();
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "AXP517 power initialization failed: %s", esp_err_to_name(error));
            (void)deinitialize();
            return error;
        }
        ESP_LOGI(kTag, "T-Panel-P4 Standard BSP initialized");
        return ESP_OK;
    }

    esp_err_t Board::deinitialize()
    {
        esp_err_t first_error = prepareForPowerDown();
        // Do not tear down the shared I2C/expander resources while an audio
        // callback could still reference the codec or I2S channels.
        if (first_error != ESP_OK &&
            (isCameraInitialized() || audio_initialized_ ||
             audio_tx_ != nullptr || audio_rx_ != nullptr))
        {
            return first_error;
        }

        // The C5/audio/LCD rail is controlled through XL9555, so disable it
        // before tearing down the expander and its I2C transport.  In
        // particular, deep-sleep teardown must cut this rail while the
        // expander is still usable.
        if (c5_audio_lcd_enabled_ && xl9555_ && xl9555_->isInitialized())
        {
            preserveFirstError(setC5AudioLcdEnabled(false), first_error);
        }

        if (spi_bus_.isInitialized())
        {
            preserveFirstError(
                detail::toEspError(spi_bus_.deinitialize()),
                first_error);
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

        if (xl9555_ && xl9555_->isInitialized())
            preserveFirstError(detail::toEspError(xl9555_->deinitialize()), first_error);
        xl9555_.reset();
        xl9555_transport_.reset();

        preserveFirstError(deinitializePower(), first_error);

        if (i2c_bus_.isInitialized())
            preserveFirstError(detail::toEspError(i2c_bus_.deinitialize()), first_error);

        initialized_ = false;
        speaker_enabled_ = false;
        c5_audio_lcd_enabled_ = false;
        return first_error;
    }

    esp_err_t Board::prepareForPowerDown(
        ShipModeProgressCallback progress_callback, void *progress_context)
    {
        const auto run_step = [&](ShipModeStep step, auto operation)
        {
            if (progress_callback != nullptr)
                progress_callback(step, false, ESP_OK, progress_context);
            const esp_err_t result = operation();
            if (progress_callback != nullptr)
                progress_callback(step, true, result, progress_context);
            return result;
        };

        esp_err_t first_error = run_step(ShipModeStep::usb_msc,
                                         [this]
                                         { return deinitializeUsbMsc(); });
        if (isUsbMscInitialized())
            return first_error;
        preserveFirstError(run_step(ShipModeStep::sd_card,
                                    [this]
                                    { return deinitializeSdCard(); }),
                           first_error);
        preserveFirstError(run_step(ShipModeStep::camera,
                                    [this]
                                    { return deinitializeCamera(); }),
                           first_error);
        // Keep the display alive while the progress list is being updated.  The
        // ship-mode path closes it after the power controller step so the user
        // can see the earlier shutdown stages complete.
        if (progress_callback == nullptr)
            preserveFirstError(run_step(ShipModeStep::display,
                                        [this]
                                        { return deinitializeDisplay(); }),
                               first_error);
        preserveFirstError(run_step(ShipModeStep::touch,
                                    [this]
                                    { return deinitializeTouch(); }),
                           first_error);
        preserveFirstError(run_step(ShipModeStep::audio,
                                    [this]
                                    { return deinitializeAudio(); }),
                           first_error);
        preserveFirstError(run_step(ShipModeStep::lora,
                                    [this]
                                    { return deinitializeLora(); }),
                           first_error);

        if (progress_callback == nullptr && xl9555_ && xl9555_->isInitialized())
        {
            preserveFirstError(
                detail::toEspError(xl9555_->writeOutputs(0)),
                first_error);
            preserveFirstError(
                detail::toEspError(xl9555_->setAllPinModes(
                    pdl::expand_io::Xl9555PinMode::output)),
                first_error);
        }
        return first_error;
    }

    esp_err_t Board::enterShipMode(std::uint32_t timeout_ms,
                                   ShipModeProgressCallback progress_callback,
                                   void *progress_context)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (timeout_ms == 0U)
            return ESP_ERR_INVALID_ARG;

        if (!isPowerInitialized())
            return ESP_ERR_INVALID_STATE;

        esp_err_t error = prepareForPowerDown(progress_callback, progress_context);
        if (error != ESP_OK)
            return error;

        if (power_interrupt_.isInitialized())
        {
            if (progress_callback != nullptr)
                progress_callback(ShipModeStep::power_interrupt, false, ESP_OK,
                                  progress_context);
            preserveFirstError(
                detail::toEspError(power_interrupt_.disable()),
                error);
            preserveFirstError(
                detail::toEspError(power_interrupt_.deinitialize()),
                error);
            if (progress_callback != nullptr)
                progress_callback(ShipModeStep::power_interrupt, true, error,
                                  progress_context);
        }
        else if (progress_callback != nullptr)
        {
            progress_callback(ShipModeStep::power_interrupt, false, ESP_OK,
                              progress_context);
            progress_callback(ShipModeStep::power_interrupt, true, ESP_OK,
                              progress_context);
        }
        stopPowerEventTask();
        if (error != ESP_OK)
            return error;

        setC5AudioLcdEnabled(true);

        ESP_LOGW(kTag, "Entering AXP517 ship mode");
        if (progress_callback != nullptr)
            progress_callback(ShipModeStep::axp517, false, ESP_OK, progress_context);
        error = detail::toEspError(axp517_->enterShipMode(timeout_ms));
        if (progress_callback != nullptr)
            progress_callback(ShipModeStep::axp517, true, error, progress_context);
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

        if (progress_callback != nullptr)
        {
            progress_callback(ShipModeStep::display, false, ESP_OK, progress_context);
            const esp_err_t display_error = deinitializeDisplay();
            progress_callback(ShipModeStep::display, true, display_error, progress_context);
            if (display_error != ESP_OK)
                return display_error;
        }

        power_ship_mode_ = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        if (progress_callback != nullptr)
            progress_callback(ShipModeStep::finalize, false, ESP_OK, progress_context);
        error = deinitialize();
        if (progress_callback != nullptr)
            progress_callback(ShipModeStep::finalize, true, error, progress_context);
        return error;
    }

    esp_err_t Board::enterLightSleep()
    {
        return enterLightSleep(LightSleepConfig{});
    }

    esp_err_t Board::enterLightSleep(const LightSleepConfig &config,
                                     SleepProgressCallback progress, void *context)
    {
        if (!initialized_)
        {
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t error = configureLightSleepWakeup(config);

        if (error != ESP_OK)
            return error;

        ESP_LOGI(
            kTag,
            "Entering light sleep: GPIO=%d active-%s, timer=%llu us",
            static_cast<int>(config.wakeup_gpio),
            config.wakeup_on_high ? "high" : "low",
            static_cast<unsigned long long>(config.timer_wakeup_us));

        const bool display_was_initialized = isDisplayInitialized();
        const bool touch_was_initialized = isTouchInitialized();
        const TouchConfig touch_config{
            touch_perform_hardware_reset_,
            touch_perform_software_reset_};
        const std::uint8_t display_frame_buffer_count = frame_buffer_count_;
        const bool display_read_display_id = display_read_display_id_;
        // setDisplaySleep(true) may have already turned the backlight off
        // while retaining the user's brightness for wakeup. Preserve that
        // value instead of capturing the temporary zero duty cycle.
        const std::uint8_t display_brightness =
            display_sleeping_ ? display_sleep_brightness_ : backlightBrightness();
        if (touch_was_initialized)
        {
            error = deinitializeTouch();
            if (error != ESP_OK)
            {
                // deinitializeTouch() releases its object storage even when
                // the I2C device removal fails. Restore the controller before
                // returning so callers never retain a board with touch left
                // unavailable after a failed sleep attempt.
                (void)initializeTouch(touch_config);
                disableLightSleepWakeup(config);
                return error;
            }
        }
        if (display_was_initialized)
        {
            error = deinitializeDisplay();
            if (error != ESP_OK)
            {
                if (touch_was_initialized)
                    (void)initializeTouch(touch_config);
                disableLightSleepWakeup(config);
                return error;
            }
        }

        // 启动轻度睡眠并获取可能的错误代码
        const esp_err_t sleep_error = esp_light_sleep_start();
        // 禁用轻度睡眠唤醒源
        disableLightSleepWakeup(config);
        // 记录日志：轻度睡眠结束，并显示唤醒原因
        ESP_LOGI(
            kTag,
            "Light sleep finished, wakeup cause=%d",
            static_cast<int>(esp_sleep_get_wakeup_cause()));

        if (!display_was_initialized && !touch_was_initialized)
            return sleep_error;

        if (display_was_initialized)
        {
            DisplayConfig display_config{};
            display_config.frame_buffer_count = display_frame_buffer_count;
            display_config.read_display_id = display_read_display_id;
            error = initializeDisplay(display_config);
            if (error == ESP_OK)
                error = setBacklightBrightness(display_brightness);
        }
        if (error == ESP_OK && touch_was_initialized)
            error = initializeTouch(touch_config);
        if (error != ESP_OK)
            ESP_LOGE(kTag, "Display/touch restore after light sleep failed: %s",
                     esp_err_to_name(error));
        return sleep_error != ESP_OK ? sleep_error : error;
    }

    esp_err_t Board::enterDeepSleep()
    {
        return enterDeepSleep(DeepSleepConfig{});
    }

    esp_err_t Board::enterDeepSleep(const DeepSleepConfig &config,
                                    SleepProgressCallback progress, void *context)
    {
        const auto notify = [&](std::uint8_t step, bool complete, esp_err_t result)
        {
            if (progress != nullptr)
                progress(step, complete, result, context);
        };
        notify(0, false, ESP_OK);
        if (!initialized_)
        {
            notify(0, true, ESP_ERR_INVALID_STATE);
            return ESP_ERR_INVALID_STATE;
        }

        esp_err_t error = configureDeepSleepWakeup(config);
        notify(0, true, error);
        if (error != ESP_OK)
            return error;
        if (config.wakeup_gpio == GPIO_NUM_NC && config.timer_wakeup_us == 0U)
        {
            ESP_LOGW(
                kTag,
                "Deep sleep has no wakeup source; reset or power cycle is required");
        }

        struct PreparationProgress
        {
            SleepProgressCallback callback;
            void *context;
        } preparation{progress, context};
        const auto preparation_progress = [](ShipModeStep step, bool complete,
                                             esp_err_t result, void *argument)
        {
            const auto &state = *static_cast<PreparationProgress *>(argument);
            if (state.callback == nullptr)
                return;
            std::uint8_t row;
            switch (step)
            {
            case ShipModeStep::sd_card:
                row = 1U;
                break;
            case ShipModeStep::camera:
                row = 2U;
                break;
            case ShipModeStep::touch:
                row = 3U;
                break;
            case ShipModeStep::audio:
                row = 4U;
                break;
            case ShipModeStep::lora:
                row = 5U;
                break;
            default:
                return;
            }
            state.callback(row, complete, result, state.context);
        };
        // Keep the display alive until the peripheral shutdown rows have rendered.
        error = prepareForPowerDown(preparation_progress, &preparation);
        if (error == ESP_OK)
        {
            notify(6, false, ESP_OK);
            error = deinitializeDisplay();
            notify(6, true, error);
        }
        if (error == ESP_OK)
        {
            notify(7, false, ESP_OK);
            error = deinitialize();
            notify(7, true, error);
        }
        if (error == ESP_OK)
        {
            notify(8, false, ESP_OK);
            error = holdDeepSleepOutputsLow();
            notify(8, true, error);
        }
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
        notify(9, false, ESP_OK);
        esp_deep_sleep_start();
    }

    bool Board::isInitialized() const noexcept
    {
        return initialized_;
    }

    esp_err_t Board::setExpanderOutput(std::uint8_t pin, bool high)
    {
        if (!initialized_ || !xl9555_ || !xl9555_->isInitialized())
            return ESP_ERR_INVALID_STATE;

        esp_err_t error = detail::toEspError(xl9555_->writePin(pin, high));
        if (error != ESP_OK)
            return error;
        return detail::toEspError(
            xl9555_->setPinMode(pin, pdl::expand_io::Xl9555PinMode::output));
    }

    esp_err_t Board::setSpeakerEnabled(bool enabled)
    {
        ESP_RETURN_ON_ERROR(
            setExpanderOutput(pins::speakerControl, enabled),
            kTag,
            "set speaker control failed");

        bool control_high{};
        ESP_RETURN_ON_ERROR(
            detail::toEspError(
                xl9555_->readPin(pins::speakerControl, control_high)),
            kTag,
            "read speaker control failed");
        ESP_RETURN_ON_FALSE(
            control_high == enabled,
            ESP_FAIL,
            kTag,
            "speaker control readback mismatch: requested=%d, actual=%d",
            enabled,
            control_high);

        ESP_LOGI(
            kTag,
            "speaker amplifier %s (XL9555 P0_0=%d)",
            enabled ? "enabled" : "disabled",
            control_high);
        speaker_enabled_ = enabled;
        return ESP_OK;
    }

    bool Board::isSpeakerEnabled() const noexcept
    {
        return speaker_enabled_;
    }

    esp_err_t Board::setSdPowerEnabled(bool enabled)
    {
        if (!enabled && isSdCardInitialized())
            return ESP_ERR_INVALID_STATE;
        return setExpanderOutput(pins::sdPowerEnable, enabled);
    }

    esp_err_t Board::setLoraPowerEnabled(bool enabled)
    {
        if (!enabled && isLoraInitialized())
            return ESP_ERR_INVALID_STATE;
        return setExpanderOutput(pins::loraPowerEnable, enabled);
    }

    esp_err_t Board::setC5AudioLcdEnabled(bool enabled)
    {
        if (!enabled && isLoraInitialized())
            return ESP_ERR_INVALID_STATE;
        const esp_err_t error =
            setExpanderOutput(pins::c5audiolcdPowerEnable, enabled);
        if (error == ESP_OK)
            c5_audio_lcd_enabled_ = enabled;
        return error;
    }

    bool Board::isC5AudioLcdEnabled() const noexcept
    {
        return c5_audio_lcd_enabled_;
    }

    esp_err_t Board::resetTouchController()
    {
        ESP_RETURN_ON_ERROR(
            setExpanderOutput(pins::touchReset, true),
            kTag,
            "set touch reset high failed");
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_RETURN_ON_ERROR(
            setExpanderOutput(pins::touchReset, false),
            kTag,
            "assert touch reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_RETURN_ON_ERROR(
            setExpanderOutput(pins::touchReset, true),
            kTag,
            "release touch reset failed");
        vTaskDelay(pdMS_TO_TICKS(200));
        return ESP_OK;
    }

    pdl::esp_idf::I2cBus *Board::i2cBus() noexcept
    {
        return initialized_ ? &i2c_bus_ : nullptr;
    }

    pdl::esp_idf::SpiBus *Board::spiBus() noexcept
    {
        return initialized_ && spi_bus_.isInitialized() ? &spi_bus_ : nullptr;
    }

    pdl::expand_io::Xl9555 *Board::ioExpander() noexcept
    {
        return initialized_ && xl9555_ ? &*xl9555_ : nullptr;
    }
}
