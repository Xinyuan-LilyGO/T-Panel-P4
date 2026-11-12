#include <bsp/board.hpp>

#include <cstdio>

#include <driver/gpio.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pdl/drivers/charge/axp517/axp517.hpp>
#include <soc/uart_pins.h>
#include "bsp_private.hpp"

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    namespace
    {
        constexpr std::uint32_t kI2cFrequencyHz = 400000;
#if T_PANEL_P4_BSP_IS_STANDARD
        constexpr char kTag[] = "t_panel_p4_standard";
#elif T_PANEL_P4_BSP_IS_ROUND
        constexpr char kTag[] = "t_panel_p4_round";
#else
        constexpr char kTag[] = "t_panel_p4_rect";
#endif

        esp_err_t logOperationError(esp_err_t error, const char *operation)
        {
            if (error != ESP_OK)
                ESP_LOGE(kTag, "%s failed: %s", operation, esp_err_to_name(error));
            return error;
        }

        constexpr gpio_num_t kDeepSleepHeldOutputs[]{
            pins::lcdBacklightPwm,
#if !T_PANEL_P4_BSP_IS_RECT
            pins::esp32c5Enable,
#endif
        };

        void releaseDeepSleepGpioHolds()
        {
            for (const gpio_num_t pin : kDeepSleepHeldOutputs)
            {
                // Backlight initialization preloads a safe low level before
                // releasing its hold. Keep it dark during early board startup.
                if (pin == pins::lcdBacklightPwm)
                    continue;
                (void)gpio_hold_dis(pin);
            }
        }

        esp_err_t holdDeepSleepOutputsLow()
        {
            for (const gpio_num_t pin : kDeepSleepHeldOutputs)
            {
                ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag,
                                    "preload deep-sleep GPIO%d low failed", static_cast<int>(pin));
                ESP_RETURN_ON_ERROR(gpio_set_direction(pin, GPIO_MODE_OUTPUT), kTag,
                                    "configure deep-sleep GPIO%d output failed", static_cast<int>(pin));
                ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), kTag, "drive deep-sleep GPIO%d low failed",
                                    static_cast<int>(pin));
                ESP_RETURN_ON_ERROR(gpio_hold_en(pin), kTag, "hold deep-sleep GPIO%d low failed",
                                    static_cast<int>(pin));
            }
            return ESP_OK;
        }

        esp_err_t configureDeepSleepConsoleIsolation()
        {
#if CONFIG_ESP_CONSOLE_UART
#if CONFIG_ESP_CONSOLE_UART_CUSTOM
            constexpr gpio_num_t console_pins[]{
                static_cast<gpio_num_t>(CONFIG_ESP_CONSOLE_UART_TX_GPIO >= 0
                    ? CONFIG_ESP_CONSOLE_UART_TX_GPIO : U0TXD_GPIO_NUM),
                static_cast<gpio_num_t>(CONFIG_ESP_CONSOLE_UART_RX_GPIO >= 0
                    ? CONFIG_ESP_CONSOLE_UART_RX_GPIO : U0RXD_GPIO_NUM)};
#else
            constexpr gpio_num_t console_pins[]{
                static_cast<gpio_num_t>(U0TXD_GPIO_NUM), static_cast<gpio_num_t>(U0RXD_GPIO_NUM)};
#endif
            // ESP-IDF excludes the console pins from automatic sleep isolation.
            // Use sleep configuration only, so the final UART logs still work.
            for (const gpio_num_t pin : console_pins)
            {
                ESP_RETURN_ON_ERROR(gpio_sleep_set_direction(pin, GPIO_MODE_DISABLE), kTag,
                                    "disable sleeping console GPIO failed");
                ESP_RETURN_ON_ERROR(gpio_sleep_set_pull_mode(pin, GPIO_FLOATING), kTag,
                                    "disable sleeping console pulls failed");
                ESP_RETURN_ON_ERROR(gpio_sleep_sel_en(pin), kTag,
                                    "enable console sleep configuration failed");
            }
            ESP_LOGI(kTag, "Deep-sleep UART isolation: TX=%d RX=%d, input/output/pulls disabled in sleep",
                     static_cast<int>(console_pins[0]), static_cast<int>(console_pins[1]));
#endif
            return ESP_OK;
        }

        void disableLightSleepWakeup(const LightSleepConfig &config)
        {
            if (config.timer_wakeup_us != 0U)
                (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
            if (config.wakeup_gpio != GPIO_NUM_NC)
            {
                (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
                (void)gpio_wakeup_disable(config.wakeup_gpio);
            }
        }

        esp_err_t configureLightSleepWakeup(const LightSleepConfig &config)
        {
            if (config.wakeup_gpio == GPIO_NUM_NC && config.timer_wakeup_us == 0U)
                return ESP_ERR_INVALID_ARG;

            if (config.wakeup_gpio != GPIO_NUM_NC)
            {
                if (!GPIO_IS_VALID_GPIO(config.wakeup_gpio))
                    return ESP_ERR_INVALID_ARG;

                ESP_RETURN_ON_ERROR(gpio_set_direction(config.wakeup_gpio, GPIO_MODE_INPUT), kTag,
                                    "configure light-sleep wakeup GPIO failed");
                if (config.wakeup_on_high)
                {
                    ESP_RETURN_ON_ERROR(gpio_pullup_dis(config.wakeup_gpio), kTag,
                                        "disable wakeup GPIO pull-up failed");
                    ESP_RETURN_ON_ERROR(gpio_pulldown_en(config.wakeup_gpio), kTag,
                                        "enable wakeup GPIO pull-down failed");
                }
                else
                {
                    ESP_RETURN_ON_ERROR(gpio_pulldown_dis(config.wakeup_gpio), kTag,
                                        "disable wakeup GPIO pull-down failed");
                    ESP_RETURN_ON_ERROR(gpio_pullup_en(config.wakeup_gpio), kTag,
                                        "enable wakeup GPIO pull-up failed");
                }

                const int active_level = config.wakeup_on_high ? 1 : 0;
                if (gpio_get_level(config.wakeup_gpio) == active_level)
                    return ESP_ERR_INVALID_STATE;

                ESP_RETURN_ON_ERROR(gpio_wakeup_enable(config.wakeup_gpio, config.wakeup_on_high
                                                                               ? GPIO_INTR_HIGH_LEVEL
                                                                               : GPIO_INTR_LOW_LEVEL),
                                    kTag, "enable light-sleep GPIO wakeup failed");
                const esp_err_t error = esp_sleep_enable_gpio_wakeup();
                if (error != ESP_OK)
                {
                    (void)gpio_wakeup_disable(config.wakeup_gpio);
                    return error;
                }
            }

            if (config.timer_wakeup_us != 0U)
            {
                const esp_err_t error = esp_sleep_enable_timer_wakeup(config.timer_wakeup_us);
                if (error != ESP_OK)
                {
                    disableLightSleepWakeup(config);
                    return error;
                }
            }
            return ESP_OK;
        }

        void disableDeepSleepWakeup(const DeepSleepConfig &config)
        {
            if (config.timer_wakeup_us != 0U)
                (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
            if (config.wakeup_gpio != GPIO_NUM_NC)
                (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_EXT1);
        }

        esp_err_t configureDeepSleepWakeup(const DeepSleepConfig &config)
        {
            if (config.wakeup_gpio != GPIO_NUM_NC)
            {
                if (!esp_sleep_is_valid_wakeup_gpio(config.wakeup_gpio))
                    return ESP_ERR_INVALID_ARG;

                ESP_RETURN_ON_ERROR(gpio_set_direction(config.wakeup_gpio, GPIO_MODE_INPUT), kTag,
                                    "configure deep-sleep wakeup GPIO failed");
                if (config.wakeup_on_high)
                {
                    ESP_RETURN_ON_ERROR(gpio_pullup_dis(config.wakeup_gpio), kTag,
                                        "disable deep-sleep wakeup pull-up failed");
                    ESP_RETURN_ON_ERROR(gpio_pulldown_en(config.wakeup_gpio), kTag,
                                        "enable deep-sleep wakeup pull-down failed");
                }
                else
                {
                    ESP_RETURN_ON_ERROR(gpio_pulldown_dis(config.wakeup_gpio), kTag,
                                        "disable deep-sleep wakeup pull-down failed");
                    ESP_RETURN_ON_ERROR(gpio_pullup_en(config.wakeup_gpio), kTag,
                                        "enable deep-sleep wakeup pull-up failed");
                }

                const int active_level = config.wakeup_on_high ? 1 : 0;
                if (gpio_get_level(config.wakeup_gpio) == active_level)
                    return ESP_ERR_INVALID_STATE;

                ESP_RETURN_ON_ERROR(
                    esp_sleep_enable_ext1_wakeup_io(std::uint64_t{1} << config.wakeup_gpio,
                                                    config.wakeup_on_high ? ESP_EXT1_WAKEUP_ANY_HIGH
                                                                          : ESP_EXT1_WAKEUP_ANY_LOW),
                    kTag, "enable deep-sleep GPIO wakeup failed");
            }

            if (config.timer_wakeup_us != 0U)
            {
                const esp_err_t error = esp_sleep_enable_timer_wakeup(config.timer_wakeup_us);
                if (error != ESP_OK)
                {
                    disableDeepSleepWakeup(config);
                    return error;
                }
            }
            return ESP_OK;
        }
    }

    namespace detail
    {
        const DisplayHardwareConfig &displayHardwareConfig() noexcept
        {
            static const DisplayHardwareConfig config = []
            {
                DisplayHardwareConfig result{};
                auto &timing = result.video_timing;
                timing.h_size = Board::displayWidth;
                timing.v_size = Board::displayHeight;
#if T_PANEL_P4_BSP_IS_STANDARD
                result.panel_name = "IVO JD9365";
                result.lane_bit_rate_mbps = 500.0F;
                result.pixel_clock_mhz = 40.0F;
                timing.hsync_pulse_width = 2;
                timing.hsync_back_porch = 44;
                timing.hsync_front_porch = 46;
                timing.vsync_pulse_width = 5;
                timing.vsync_back_porch = 16;
                timing.vsync_front_porch = 50;
#elif T_PANEL_P4_BSP_IS_ROUND
                result.panel_name = "BOE JD9365DA-H3";
                result.lane_bit_rate_mbps = 500.0F;
                result.pixel_clock_mhz = 36.0F;
                timing.hsync_pulse_width = 2;
                timing.hsync_back_porch = 22;
                timing.hsync_front_porch = 68;
                timing.vsync_pulse_width = 5;
                timing.vsync_back_porch = 16;
                timing.vsync_front_porch = 50;
#else
                result.panel_name = "HD0549S001 ILI9882";
                result.lane_bit_rate_mbps = 1200.0F;
                result.pixel_clock_mhz = 72.0F;
                timing.hsync_pulse_width = 4;
                timing.hsync_back_porch = 52;
                timing.hsync_front_porch = 48;
                timing.vsync_pulse_width = 4;
                timing.vsync_back_porch = 20;
                timing.vsync_front_porch = 216;
#endif
                return result;
            }();
            return config;
        }

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

    esp_err_t Board::initializeI2cBus()
    {
        if (i2c_bus_.isInitialized())
            return ESP_OK;

        releaseDeepSleepGpioHolds();
        pdl::esp_idf::I2cBus::Config bus_config{};
        bus_config.sda = pins::i2cSda;
        bus_config.scl = pins::i2cScl;
        bus_config.enable_internal_pullup = true;
        const esp_err_t error = detail::toEspError(i2c_bus_.initialize(bus_config));
        if (error != ESP_OK)
            ESP_LOGE(kTag, "I2C initialization failed: %s", esp_err_to_name(error));
        return error;
    }

    bool Board::isI2cBusInitialized() const noexcept
    {
        return i2c_bus_.isInitialized();
    }

#if !T_PANEL_P4_BSP_IS_RECT
    esp_err_t Board::initializeIoExpander()
    {
        if (!isI2cBusInitialized())
        {
            ESP_RETURN_ON_ERROR(initializeI2cBus(), kTag, "initialize shared I2C failed");
        }
        if (isIoExpanderInitialized())
            return ESP_OK;

        // Release a previous incomplete attempt before binding a new transport.
        xl9555_.reset();
        xl9555_transport_.reset();

        xl9555_transport_.emplace(i2c_bus_, pins::xl9555Address, kI2cFrequencyHz);
        xl9555_.emplace(*xl9555_transport_);

        esp_err_t error = detail::toEspError(xl9555_->initialize());
        // Preload both output ports low before switching all 16 pins to outputs.
        if (error == ESP_OK)
            error = detail::toEspError(xl9555_->writeOutputs(0));
        if (error == ESP_OK)
            error = detail::toEspError(xl9555_->setAllPinModes(pdl::expand_io::Xl9555PinMode::output));

        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "XL9555 initialization failed: %s", esp_err_to_name(error));
            xl9555_.reset();
            xl9555_transport_.reset();
        }

        return error;
    }
#endif

    esp_err_t Board::initializePower()
    {
        return initializePower(PowerConfig{});
    }

    esp_err_t Board::initializePower(const PowerConfig &config)
    {
        ESP_RETURN_ON_ERROR(initializePowerController(config), kTag, "initialize AXP517 failed");

#if !T_PANEL_P4_BSP_IS_RECT
        ESP_RETURN_ON_ERROR(initializeIoExpander(), kTag, "initialize power IO expander failed");
        if (!isC5AudioLcdEnabled())
        {
            ESP_RETURN_ON_ERROR(setC5AudioLcdEnabled(true), kTag, "enable peripheral power failed");
        }
        return gpio_set_level(pins::esp32c5Enable, 1);
#else
        return ESP_OK;
#endif
    }

    esp_err_t Board::initialize()
    {
        if (isInitialized())
            return ESP_OK;

        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize board power failed");
        struct Step
        {
            const char *name;
            bool ready;
            esp_err_t (Board::*initialize)();
            esp_err_t (Board::*deinitialize)();
        };
        const Step steps[]{
            {"Display", isDisplayInitialized(), &Board::initializeDisplay, &Board::deinitializeDisplay},
            {"Touch", isTouchInitialized(), &Board::initializeTouch, &Board::deinitializeTouch},
            {"Audio", isAudioInitialized(), &Board::initializeAudio, &Board::deinitializeAudio},
            {"Camera", isCameraInitialized(), &Board::initializeCamera, &Board::deinitializeCamera},
#if !T_PANEL_P4_BSP_IS_RECT
            {"Lora", isLoraInitialized(), &Board::initializeLora, &Board::deinitializeLora},
#endif
            {"SdCard", isSdCardInitialized(), &Board::initializeSdCard, &Board::deinitializeSdCard},
        };
        for (std::size_t index = 0; index < sizeof(steps) / sizeof(steps[0]); ++index)
        {
            if (steps[index].ready)
                continue;
            const esp_err_t error = (this->*steps[index].initialize)();
            if (error == ESP_OK)
                continue;
            ESP_LOGE(kTag, "%s initialization failed: %s", steps[index].name,
                     esp_err_to_name(error));
            // Include the failing step in case it has resources left to release.
            // Devices that were already ready before this call are never touched.
            for (std::size_t rollback = index + 1; rollback > 0; --rollback)
            {
                const Step &step = steps[rollback - 1];
                if (step.ready)
                    continue;
                const esp_err_t cleanup = (this->*step.deinitialize)();
                if (cleanup != ESP_OK)
                    ESP_LOGE(kTag, "rollback %s failed: %s", step.name, esp_err_to_name(cleanup));
            }
            return error;
        }
        ESP_LOGI(kTag, "All board peripherals initialized");
        return ESP_OK;
    }

    esp_err_t Board::deinitialize()
    {
        ESP_RETURN_ON_ERROR(prepareForPowerDown(), kTag, "prepare board power-down failed");
#if !T_PANEL_P4_BSP_IS_RECT
        // Disable the shared rail before releasing the expander and I2C transport.
        if (isIoExpanderInitialized())
        {
            ESP_RETURN_ON_ERROR(setC5AudioLcdEnabled(false), kTag,
                                "disable C5/audio/LCD power failed");
        }
        if (spi_bus_.isInitialized())
        {
            ESP_RETURN_ON_ERROR(detail::toEspError(spi_bus_.deinitialize()), kTag,
                                "deinitialize SPI bus failed");
        }
        if (isIoExpanderInitialized())
        {
            // Read hardware, not the driver's cached latches/modes, before
            // releasing the last way to verify the power-enable outputs.
            const auto read_register_pair = [&](std::uint8_t reg, std::uint16_t &value)
            {
                std::uint8_t bytes[2]{};
                const esp_err_t error = detail::toEspError(xl9555_transport_->writeRead(
                    &reg, sizeof(reg), bytes, sizeof(bytes), 100));
                if (error == ESP_OK)
                    value = static_cast<std::uint16_t>(bytes[0]) |
                            (static_cast<std::uint16_t>(bytes[1]) << 8U);
                return error;
            };
            std::uint16_t outputs{}, modes{}, levels{};
            ESP_RETURN_ON_ERROR(read_register_pair(0x02, outputs), kTag,
                                "read XL9555 output latches before shutdown failed");
            ESP_RETURN_ON_ERROR(read_register_pair(0x06, modes), kTag,
                                "read XL9555 pin modes before shutdown failed");
            ESP_RETURN_ON_ERROR(detail::toEspError(xl9555_->readInputs(levels)), kTag,
                                "read XL9555 pin levels before shutdown failed");
            levels ^= xl9555_->polarityInversionMask();
            ESP_LOGI(kTag, "XL9555 shutdown readback: OUT=0x%04X CFG=0x%04X LEVEL=0x%04X",
                     static_cast<unsigned>(outputs), static_cast<unsigned>(modes),
                     static_cast<unsigned>(levels));
            ESP_RETURN_ON_FALSE(outputs == 0U && modes == 0U && levels == 0U,
                                ESP_ERR_INVALID_STATE, kTag,
                                "XL9555 shutdown requires all 16 pins driven low");
            ESP_RETURN_ON_ERROR(detail::toEspError(xl9555_->deinitialize()), kTag,
                                "deinitialize XL9555 failed");
        }
        xl9555_.reset();
        xl9555_transport_.reset();
#endif
        ESP_RETURN_ON_ERROR(deinitializeBacklight(), kTag, "deinitialize backlight failed");
        // AXP517 still owns an I2C device and an interrupt worker. Release
        // both before deleting their shared bus.
        ESP_RETURN_ON_ERROR(deinitializePower(), kTag, "deinitialize power controller failed");
        if (i2c_bus_.isInitialized())
        {
            ESP_RETURN_ON_ERROR(detail::toEspError(i2c_bus_.deinitialize()), kTag,
                                "deinitialize I2C bus failed");
        }
        return ESP_OK;
    }

    esp_err_t Board::prepareForPowerDown()
    {
        ESP_RETURN_ON_ERROR(deinitializeUsbMsc(), kTag, "deinitialize USB MSC failed");
        ESP_RETURN_ON_FALSE(!isUsbMscInitialized(), ESP_ERR_INVALID_STATE, kTag, "USB MSC is still initialized");
        ESP_RETURN_ON_ERROR(deinitializeSdCard(), kTag, "deinitialize SD card failed");
        ESP_RETURN_ON_ERROR(deinitializeCamera(), kTag, "deinitialize camera failed");
        ESP_RETURN_ON_ERROR(deinitializeDisplay(), kTag, "deinitialize display failed");
        ESP_RETURN_ON_ERROR(deinitializeTouch(), kTag, "deinitialize touch failed");
        ESP_RETURN_ON_ERROR(deinitializeAudio(), kTag, "deinitialize audio failed");
#if !T_PANEL_P4_BSP_IS_RECT
        ESP_RETURN_ON_ERROR(deinitializeLora(), kTag, "deinitialize LoRa failed");
        if (isIoExpanderInitialized())
        {
            ESP_RETURN_ON_ERROR(detail::toEspError(xl9555_->writeOutputs(0)), kTag,
                                "clear XL9555 outputs failed");
            ESP_RETURN_ON_ERROR(
                detail::toEspError(xl9555_->setAllPinModes(pdl::expand_io::Xl9555PinMode::output)),
                kTag, "configure XL9555 outputs failed");
        }
#endif
        return ESP_OK;
    }

    esp_err_t Board::enterShipMode(std::uint32_t timeout_ms)
    {
        ESP_RETURN_ON_FALSE(isPowerInitialized(), ESP_ERR_INVALID_STATE, kTag, "ship mode requires initialized power controller");
        ESP_RETURN_ON_FALSE(timeout_ms != 0U, ESP_ERR_INVALID_ARG, kTag, "ship-mode timeout must be nonzero");
        ESP_LOGI(kTag, "Entering AXP517 ship mode");
        ESP_RETURN_ON_ERROR(detail::toEspError(axp517_->enterShipMode(timeout_ms)), kTag,
                            "AXP517 enter ship mode failed");
        return ESP_OK;
    }

    esp_err_t Board::enterLightSleep()
    {
        return enterLightSleep(LightSleepConfig{});
    }

    esp_err_t Board::enterLightSleep(const LightSleepConfig &config)
    {
        ESP_RETURN_ON_FALSE(isPowerInitialized(), ESP_ERR_INVALID_STATE, kTag,
                            "light sleep requires initialized power controller");

        ESP_RETURN_ON_ERROR(configureLightSleepWakeup(config), kTag,
                            "configure light-sleep wakeup failed");
        esp_err_t error = ESP_OK;

        ESP_LOGI(kTag, "Entering light sleep: GPIO=%d active-%s, timer=%llu us",
                 static_cast<int>(config.wakeup_gpio), config.wakeup_on_high ? "high" : "low",
                 static_cast<unsigned long long>(config.timer_wakeup_us));
        const bool display_was_initialized = isDisplayInitialized();
        const bool touch_was_initialized = isTouchInitialized();
        const TouchConfig touch_config = touchConfig();
        const DisplayConfig display_config = displayConfig();
        const std::uint8_t display_brightness = backlightBrightness();

        if (touch_was_initialized)
        {
            error = logOperationError(deinitializeTouch(), "deinitialize touch before light sleep");
            if (error != ESP_OK)
            {
                // deinitializeTouch() releases its object storage even when
                // the I2C device removal fails. Restore the controller before
                // returning so callers never retain a board with touch left
                // unavailable after a failed sleep attempt.
                (void)logOperationError(initializeTouch(touch_config),
                                        "restore touch after failed light-sleep preparation");
                disableLightSleepWakeup(config);
                return error;
            }
        }
        if (display_was_initialized)
        {
            error = logOperationError(deinitializeDisplay(), "deinitialize display before light sleep");
            if (error != ESP_OK)
            {
                if (touch_was_initialized)
                    (void)logOperationError(initializeTouch(touch_config),
                                            "restore touch after failed light-sleep preparation");
                disableLightSleepWakeup(config);
                return error;
            }
        }

        const esp_err_t sleep_error = logOperationError(esp_light_sleep_start(), "enter light sleep");
        disableLightSleepWakeup(config);
        ESP_LOGI(kTag, "Light sleep finished, wakeup cause=%d",
                 static_cast<int>(esp_sleep_get_wakeup_cause()));
        if (!display_was_initialized && !touch_was_initialized)
            return sleep_error;

        if (display_was_initialized)
        {
            error = logOperationError(initializeDisplay(display_config), "restore display after light sleep");
            if (error == ESP_OK)
                error = logOperationError(setBacklightBrightness(display_brightness),
                                          "restore backlight after light sleep");
        }
        if (error == ESP_OK && touch_was_initialized)
            error = logOperationError(initializeTouch(touch_config), "restore touch after light sleep");
        return sleep_error != ESP_OK ? sleep_error : error;
    }

    esp_err_t Board::enterDeepSleep()
    {
        return enterDeepSleep(DeepSleepConfig{});
    }

    esp_err_t Board::enterDeepSleep(const DeepSleepConfig &config)
    {
        ESP_RETURN_ON_FALSE(isPowerInitialized(), ESP_ERR_INVALID_STATE, kTag,
                            "deep sleep requires initialized power controller");
        ESP_RETURN_ON_ERROR(configureDeepSleepWakeup(config), kTag,
                            "configure deep-sleep wakeup failed");
        if (config.wakeup_gpio == GPIO_NUM_NC && config.timer_wakeup_us == 0U)
            ESP_LOGW(kTag, "Deep sleep has no wakeup source; reset or power cycle is required");

        esp_err_t error = deinitialize();
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "deinitialize board before deep sleep failed: %s",
                     esp_err_to_name(error));
            disableDeepSleepWakeup(config);
            return error;
        }
        error = holdDeepSleepOutputsLow();
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "hold deep-sleep outputs low failed: %s", esp_err_to_name(error));
            disableDeepSleepWakeup(config);
            return error;
        }
        error = configureDeepSleepConsoleIsolation();
        if (error != ESP_OK)
        {
            disableDeepSleepWakeup(config);
            return error;
        }
        ESP_LOGI(kTag, "LDO references before deep sleep (Flash/PSRAM power-down is handled by ESP-IDF)");
        (void)esp_ldo_dump(stdout);
        ESP_LOGI(kTag, "Entering deep sleep: GPIO=%d active-%s, timer=%llu us",
                 static_cast<int>(config.wakeup_gpio), config.wakeup_on_high ? "high" : "low",
                 static_cast<unsigned long long>(config.timer_wakeup_us));
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_deep_sleep_start();
    }

#if !T_PANEL_P4_BSP_IS_RECT
    bool Board::isIoExpanderInitialized() const noexcept
    {
        return xl9555_ && xl9555_->isInitialized();
    }

    esp_err_t Board::setExpanderOutput(std::uint8_t pin, bool high)
    {
        if (!xl9555_ || !xl9555_->isInitialized())
            return ESP_ERR_INVALID_STATE;

        esp_err_t error = detail::toEspError(xl9555_->writePin(pin, high));
        if (error != ESP_OK)
            return error;
        return detail::toEspError(xl9555_->setPinMode(pin, pdl::expand_io::Xl9555PinMode::output));
    }
#endif

    esp_err_t Board::setControlOutput(int pin, bool high, bool isIOExpander)
    {
        ESP_RETURN_ON_FALSE(pin != GPIO_NUM_NC, ESP_ERR_NOT_SUPPORTED, kTag,
                            "control pin is not connected");
        if (isIOExpander)
        {
#if !T_PANEL_P4_BSP_IS_RECT
            ESP_RETURN_ON_FALSE(pin >= 0 && pin < 16, ESP_ERR_INVALID_ARG, kTag, "invalid XL9555 pin %d", pin);
            const auto expander_pin = static_cast<std::uint8_t>(pin);
            ESP_RETURN_ON_ERROR(setExpanderOutput(expander_pin, high), kTag, "set XL9555 pin %d failed", pin);
            bool actual_high{};
            ESP_RETURN_ON_ERROR(detail::toEspError(xl9555_->readPin(expander_pin, actual_high)), kTag, "read XL9555 pin %d failed", pin);
            ESP_RETURN_ON_FALSE(actual_high == high, ESP_FAIL, kTag, "XL9555 pin %d readback mismatch: requested=%d, actual=%d", pin, high, actual_high);
#else
            ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, kTag, "XL9555 is unavailable on Rect");
#endif
        }
        else
        {
            ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(pin), ESP_ERR_INVALID_ARG, kTag, "invalid control GPIO%d", pin);
            const auto gpio = static_cast<gpio_num_t>(pin);
            ESP_RETURN_ON_ERROR(gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT), kTag, "configure control GPIO%d failed", pin);
            ESP_RETURN_ON_ERROR(gpio_set_level(gpio, high ? 1 : 0), kTag, "set control GPIO%d failed", pin);
        }
        return ESP_OK;
    }

    bool Board::controlPinHigh(int pin, bool isIOExpander) const noexcept
    {
        if (pin == GPIO_NUM_NC)
            return false;
        if (isIOExpander)
        {
#if !T_PANEL_P4_BSP_IS_RECT
            if (pin < 0 || pin >= 16 || !isIoExpanderInitialized())
                return false;
            bool high{};
            return pdl::isOk(xl9555_->readPin(static_cast<std::uint8_t>(pin), high)) && high;
#else
            return false;
#endif
        }
        return GPIO_IS_VALID_GPIO(pin) && gpio_get_level(static_cast<gpio_num_t>(pin)) != 0;
    }

    bool Board::isInitialized() const noexcept
    {
        return isPowerInitialized() && isDisplayInitialized() && isTouchInitialized() &&
               isAudioInitialized() && isCameraInitialized() &&
#if !T_PANEL_P4_BSP_IS_RECT
               isLoraInitialized() &&
#endif
               isSdCardInitialized();
    }

    esp_err_t Board::setSpeakerEnabled(bool enabled, bool isIOExpander)
    {
        ESP_RETURN_ON_ERROR(setControlOutput(pins::speakerControl, enabled, isIOExpander), kTag, "set speaker control failed");
        ESP_LOGI(kTag, "speaker amplifier %s (%s pin %u)", enabled ? "enabled" : "disabled", isIOExpander ? "XL9555" : "GPIO", static_cast<unsigned>(pins::speakerControl));
        return ESP_OK;
    }

    bool Board::isSpeakerEnabled(bool isIOExpander) const noexcept
    {
        return controlPinHigh(pins::speakerControl, isIOExpander);
    }

    esp_err_t Board::setSdPowerEnabled(bool enabled, bool isIOExpander)
    {
        if (!enabled && isSdCardInitialized())
            return ESP_ERR_INVALID_STATE;
#if !T_PANEL_P4_BSP_IS_RECT
        ESP_RETURN_ON_ERROR(setControlOutput(pins::sdPowerEnable, enabled, isIOExpander), kTag, "set SD power failed");
#else
        ESP_RETURN_ON_FALSE(!isIOExpander, ESP_ERR_NOT_SUPPORTED, kTag, "XL9555 is unavailable on Rect");
#endif
        return ESP_OK;
    }

    esp_err_t Board::setLoraPowerEnabled(bool enabled, bool isIOExpander)
    {
#if !T_PANEL_P4_BSP_IS_RECT
        if (!enabled && isLoraInitialized())
            return ESP_ERR_INVALID_STATE;
#endif
        ESP_RETURN_ON_ERROR(setControlOutput(pins::loraPowerEnable, enabled, isIOExpander), kTag, "set LoRa power failed");
        return ESP_OK;
    }

    esp_err_t Board::setC5AudioLcdEnabled(bool enabled, bool isIOExpander)
    {
        ESP_RETURN_ON_ERROR(setControlOutput(pins::c5audiolcdPowerEnable, enabled, isIOExpander), kTag, "set C5/audio/LCD power-enable failed");
        return ESP_OK;
    }

    bool Board::isC5AudioLcdEnabled(bool isIOExpander) const noexcept
    {
        return controlPinHigh(pins::c5audiolcdPowerEnable, isIOExpander);
    }

#if T_PANEL_P4_BSP_IS_RECT
    esp_err_t Board::setDisplayPowerEnabled(bool enabled, bool isIOExpander)
    {
        ESP_RETURN_ON_ERROR(setControlOutput(pins::lcdPower, enabled, isIOExpander), kTag,
                            "set display power failed");
        ESP_LOGI(kTag, "display power %s (%s pin %u)", enabled ? "enabled" : "disabled",
                 isIOExpander ? "XL9555" : "GPIO", static_cast<unsigned>(pins::lcdPower));
        return ESP_OK;
    }

    bool Board::isDisplayPowerEnabled(bool isIOExpander) const noexcept
    {
        return controlPinHigh(pins::lcdPower, isIOExpander);
    }
#endif

    esp_err_t Board::resetTouchController(bool isIOExpander)
    {
        return resetTouchControllerImpl(isIOExpander, true);
    }

    esp_err_t Board::resetTouchControllerImpl(bool isIOExpander, bool restore_interrupt_input)
    {
        // Configure RST once; subsequent steps only change its output level.
        ESP_RETURN_ON_ERROR(setControlOutput(pins::touchReset, !T_PANEL_P4_BSP_IS_ROUND, isIOExpander),
                            kTag, "configure touch reset failed");
        const auto set_reset = [this, isIOExpander](bool high) -> esp_err_t
        {
#if !T_PANEL_P4_BSP_IS_RECT
            if (isIOExpander)
                return detail::toEspError(xl9555_->writePin(pins::touchReset, high));
#endif
            return gpio_set_level(static_cast<gpio_num_t>(pins::touchReset), high ? 1 : 0);
        };
#if T_PANEL_P4_BSP_IS_ROUND
        // Match the round reference sequence: RST low >=10 ms, select INT,
        // release RST, then release INT directly to the controller.
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_config_t select_config{};
        select_config.pin_bit_mask = std::uint64_t{1} << pins::touchInterrupt;
        select_config.mode = GPIO_MODE_OUTPUT;
        select_config.intr_type = GPIO_INTR_DISABLE;
        ESP_RETURN_ON_ERROR(gpio_config(&select_config), kTag,
                            "configure GT911 address-select output failed");
        // GT911 latches its 7-bit address from INT when RST is released.
        ESP_RETURN_ON_ERROR(gpio_set_level(pins::touchInterrupt,
                                           touch_config_.i2c_address == pdl::touch::Gt911Touch::alternateI2cAddress ? 1 : 0),
                            kTag, "set GT911 address-select level failed");
        // Round up so a 2 ms hold does not become zero at a 100 Hz tick rate.
        vTaskDelay(pdMS_TO_TICKS(2) + 1);
#else
        vTaskDelay(pdMS_TO_TICKS(5));
        ESP_RETURN_ON_ERROR(set_reset(false), kTag, "assert touch reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
        ESP_RETURN_ON_ERROR(set_reset(true), kTag, "release touch reset failed");
#if T_PANEL_P4_BSP_IS_ROUND
        vTaskDelay(pdMS_TO_TICKS(10));
        if (restore_interrupt_input)
        {
            ESP_RETURN_ON_ERROR(gpio_set_direction(pins::touchInterrupt, GPIO_MODE_INPUT),
                                kTag, "release GT911 address-select output failed");
            vTaskDelay(pdMS_TO_TICKS(50));
        }
#else
        vTaskDelay(pdMS_TO_TICKS(200));
#endif
        return ESP_OK;
    }

    Devices Board::devices() noexcept
    {
        Devices result{};
        result.i2c_bus = i2cBus();
        result.axp517 = isPowerInitialized() ? &*axp517_ : nullptr;
#if !T_PANEL_P4_BSP_IS_RECT
        result.spi_bus = spiBus();
        result.xl9555 = ioExpander();
        result.lr2021 = loraRadio();
#endif
        result.es8389 = isAudioInitialized() ? &es8389_ : nullptr;
        result.audio_codec = audioCodec();
        result.audio_input_codec = audioInputCodec();
        result.touch = touchController();
        result.display_panel = isDisplayInitialized() ? displayPanel() : nullptr;
        result.dsi_bus = isDisplayInitialized() ? dsi_bus_ : nullptr;
        result.display_io = isDisplayInitialized() ? dbi_io_ : nullptr;
        result.i2s_tx = audioTransmitChannel();
        result.i2s_rx = audioReceiveChannel();
        result.sd_card = sdCard();
        populateCameraDevices(result);
        return result;
    }

    pdl::esp_idf::I2cBus *Board::i2cBus() noexcept
    {
        return i2c_bus_.isInitialized() ? &i2c_bus_ : nullptr;
    }

#if !T_PANEL_P4_BSP_IS_RECT
    pdl::esp_idf::SpiBus *Board::spiBus() noexcept
    {
        return spi_bus_.isInitialized() ? &spi_bus_ : nullptr;
    }

    pdl::expand_io::Xl9555 *Board::ioExpander() noexcept
    {
        return isIoExpanderInitialized() ? &*xl9555_ : nullptr;
    }
#endif
}
