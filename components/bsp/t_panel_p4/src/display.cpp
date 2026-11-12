#include <bsp/board.hpp>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_private/esp_gpio_reserve.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/mipi_dsi_ll.h>

#if T_PANEL_P4_BSP_IS_STANDARD
#include <pdl/drivers/display/jd9365/jd9365.hpp>
#elif T_PANEL_P4_BSP_IS_ROUND
#include <pdl/drivers/display/jd9365da/jd9365da.hpp>
#else
#include <pdl/drivers/display/ili9882/ili9882.hpp>
#endif

#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_display";
    constexpr int kDsiBusId = 0;
    constexpr std::uint8_t kDsiLaneCount = 2;
    constexpr int kMipiPhyLdoChannel = 3;
    constexpr int kMipiPhyVoltageMv = 2500;
    constexpr std::uint32_t kDisplayResetReleaseDelayMs = 250;
    constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
    constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
    constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_11_BIT;
    constexpr std::uint32_t kBacklightFrequencyHz = 30000;

    lcd_color_format_t panelColorFormat(std::uint8_t bits_per_pixel)
    {
        switch (bits_per_pixel)
        {
        case 16: return LCD_COLOR_FMT_RGB565;
        case 18: return LCD_COLOR_FMT_RGB666;
        case 24: return LCD_COLOR_FMT_RGB888;
        default: return {};
        }
    }

    const char *colorFormatName(lcd_color_format_t format)
    {
        switch (format)
        {
        case LCD_COLOR_FMT_RGB565: return "RGB565";
        case LCD_COLOR_FMT_RGB666: return "RGB666";
        case LCD_COLOR_FMT_RGB888: return "RGB888";
        default: return "unsupported";
        }
    }

    void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
    {
        if (first_error == ESP_OK && candidate != ESP_OK)
            first_error = candidate;
    }
}

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    esp_err_t Board::initializeBacklight()
    {
        if (backlight_initialized_)
            return ESP_OK;

        ESP_RETURN_ON_ERROR(gpio_set_level(pins::lcdBacklightPwm, 0), kTag,
                            "drive backlight low failed");
        ESP_RETURN_ON_ERROR(gpio_set_direction(pins::lcdBacklightPwm, GPIO_MODE_OUTPUT), kTag,
                            "configure backlight GPIO failed");
        ESP_RETURN_ON_ERROR(gpio_set_pull_mode(pins::lcdBacklightPwm, GPIO_FLOATING), kTag,
                            "disable backlight GPIO pulls failed");
        ESP_RETURN_ON_ERROR(gpio_hold_dis(pins::lcdBacklightPwm), kTag,
                            "release backlight GPIO hold failed");

        ledc_timer_config_t timer_config{};
        timer_config.speed_mode = kBacklightSpeedMode;
        timer_config.duty_resolution = kBacklightResolution;
        timer_config.timer_num = kBacklightTimer;
        timer_config.freq_hz = kBacklightFrequencyHz;
        timer_config.clk_cfg = LEDC_AUTO_CLK;
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag,
                            "configure backlight timer failed");

        ledc_channel_config_t channel_config{};
        channel_config.gpio_num = pins::lcdBacklightPwm;
        channel_config.speed_mode = kBacklightSpeedMode;
        channel_config.channel = kBacklightChannel;
        channel_config.timer_sel = kBacklightTimer;
        channel_config.duty = 0;
        channel_config.hpoint = 0;
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), kTag,
                            "configure backlight channel failed");

        backlight_initialized_ = true;
        backlight_brightness_ = 0;
        return ESP_OK;
    }

    esp_err_t Board::deinitializeBacklight()
    {
        if (!backlight_initialized_)
            return ESP_OK;
        esp_err_t first_error = setBacklightBrightness(0);
        preserveFirstError(ledc_stop(kBacklightSpeedMode, kBacklightChannel, 0), first_error);
        // gpio_reset_pin() disables the output and enables a pull-up, turning
        // the active-high backlight back on. Disconnect LEDC, drive the pad low
        // and hold it there while light sleep powers down the peripheral clock.
        preserveFirstError(gpio_set_level(pins::lcdBacklightPwm, 0), first_error);
        preserveFirstError(gpio_set_direction(pins::lcdBacklightPwm, GPIO_MODE_OUTPUT), first_error);
        // ledc_stop() leaves its GPIO reservation in place. The output is now
        // disconnected from LEDC, so release ownership before a later rebind.
        esp_gpio_revoke(std::uint64_t{1} << pins::lcdBacklightPwm);
        preserveFirstError(gpio_set_pull_mode(pins::lcdBacklightPwm, GPIO_PULLDOWN_ONLY), first_error);
        preserveFirstError(gpio_hold_en(pins::lcdBacklightPwm), first_error);
        backlight_initialized_ = false;
        backlight_brightness_ = 0;
        if (first_error == ESP_OK)
            ESP_LOGI(kTag, "LCD backlight off: GPIO%d held low", static_cast<int>(pins::lcdBacklightPwm));
        return first_error;
    }

    esp_err_t Board::setBacklightBrightness(std::uint8_t percent)
    {
        if (!backlight_initialized_)
            return ESP_ERR_INVALID_STATE;
        if (percent > 100U)
            return ESP_ERR_INVALID_ARG;

        constexpr std::uint32_t maximum_duty = (std::uint32_t{1} << kBacklightResolution) - 1U;
        const std::uint32_t duty = maximum_duty * percent / 100U;
        ESP_RETURN_ON_ERROR(ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty), kTag,
                            "set backlight duty failed");
        ESP_RETURN_ON_ERROR(ledc_update_duty(kBacklightSpeedMode, kBacklightChannel), kTag,
                            "update backlight duty failed");

        backlight_brightness_ = percent;
        return ESP_OK;
    }

    std::uint8_t Board::backlightBrightness() const noexcept
    {
        return backlight_brightness_;
    }

#if T_PANEL_P4_BSP_IS_STANDARD
    esp_err_t Board::setDisplaySleep(bool sleeping)
    {
        if (!panel_initialized_)
            return ESP_ERR_INVALID_STATE;
        if (sleeping == display_sleeping_)
            return ESP_OK;

        if (sleeping)
        {
            display_sleep_brightness_ = backlightBrightness();
            // Stop at the LVGL-provided frame boundary, suspend the DPI GDMA
            // channel, and leave the panel command path untouched until wakeup.
            // The visible sleep behavior is provided by the backlight.
            esp_err_t error = pdl::display::suspendJd9365Panel(panel_);
            if (error == ESP_OK)
                error = setBacklightBrightness(0);
            if (error != ESP_OK)
            {
                ESP_LOGE(kTag, "DPI/display suspend failed: %s", esp_err_to_name(error));
                (void)pdl::display::resumeJd9365Panel(panel_);
                (void)setBacklightBrightness(display_sleep_brightness_);
                return error;
            }
            display_sleeping_ = true;
            ESP_LOGI(kTag, "DPI/GDMA suspended, LCD backlight off");
            return ESP_OK;
        }

        // Re-synchronize JD9365 with write-only commands, reload the existing DPI
        // descriptor from framebuffer zero, then restore the backlight. No panel
        // register read and no second esp_lcd_panel_init() are performed here.
        esp_err_t error = pdl::display::resumeJd9365Panel(panel_);
        if (error == ESP_OK)
            error = setBacklightBrightness(display_sleep_brightness_);
        if (error == ESP_OK)
            display_sleeping_ = false;
        if (error != ESP_OK)
            ESP_LOGE(kTag, "DPI/JD9365 resume failed: %s", esp_err_to_name(error));
        else
            ESP_LOGI(kTag, "DPI/GDMA resumed, JD9365 synchronized");
        return error;
    }
#endif

    esp_err_t Board::assertDisplayReset(bool isIOExpander)
    {
        ESP_RETURN_ON_ERROR(setControlOutput(pins::lcdReset, false, isIOExpander), kTag,
                            "assert LCD reset failed");
        vTaskDelay(pdMS_TO_TICKS(20));
        return ESP_OK;
    }

    esp_err_t Board::releaseDisplayReset(bool isIOExpander)
    {
        ESP_RETURN_ON_ERROR(setControlOutput(pins::lcdReset, true, isIOExpander), kTag,
                            "release LCD reset failed");
        vTaskDelay(pdMS_TO_TICKS(kDisplayResetReleaseDelayMs));
        return ESP_OK;
    }

    esp_err_t Board::initializeDisplay()
    {
        return initializeDisplay(DisplayConfig{});
    }

    esp_err_t Board::initializeDisplay(const DisplayConfig &config)
    {
        if (display_initialized_)
            return ESP_OK;
        // A failed teardown retains live resources. Finish that teardown
        // before allocating replacements over the existing handles.
        if (panel_ != nullptr || dbi_io_ != nullptr || dsi_bus_ != nullptr || mipi_phy_ldo_ != nullptr)
            return ESP_ERR_INVALID_STATE;
        if (config.frame_buffer_count == 0U || config.frame_buffer_count > maximumFrameBufferCount)
        {
            return ESP_ERR_INVALID_ARG;
        }
        const lcd_color_format_t out_color_format = panelColorFormat(config.bits_per_pixel);
        if (out_color_format == lcd_color_format_t{} ||
            (config.in_color_format != LCD_COLOR_FMT_RGB565 &&
             config.in_color_format != LCD_COLOR_FMT_RGB666 &&
             config.in_color_format != LCD_COLOR_FMT_RGB888))
        {
            return ESP_ERR_NOT_SUPPORTED;
        }
        const auto &hardware = detail::displayHardwareConfig();

        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize device power failed");

        esp_err_t error = initializeBacklight();
        if (error == ESP_OK)
            error = setBacklightBrightness(0);
        if (error == ESP_OK)
            error = assertDisplayReset();
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }

        esp_ldo_channel_config_t ldo_config{};
        ldo_config.chan_id = kMipiPhyLdoChannel;
        ldo_config.voltage_mv = kMipiPhyVoltageMv;
        error = esp_ldo_acquire_channel(&ldo_config, &mipi_phy_ldo_);
        if (error == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(kTag, "MIPI PHY LDO channel is already enabled");
            mipi_phy_ldo_ = nullptr;
            error = ESP_OK;
        }
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }
        ESP_LOGI(kTag, "Waiting for MIPI PHY LDO to stabilize");
        vTaskDelay(pdMS_TO_TICKS(50));

        esp_lcd_dsi_bus_config_t bus_config{};
        bus_config.bus_id = kDsiBusId;
        bus_config.num_data_lanes = kDsiLaneCount;
        // Let ESP-IDF select the revision-appropriate DSI PHY reference clock.
        bus_config.phy_clk_src = {};
        bus_config.lane_bit_rate_mbps = hardware.lane_bit_rate_mbps;
        error = esp_lcd_new_dsi_bus(&bus_config, &dsi_bus_);
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }

        esp_lcd_dbi_io_config_t dbi_config{};
        dbi_config.virtual_channel = 0;
        dbi_config.lcd_cmd_bits = 8;
        dbi_config.lcd_param_bits = 8;
        error = esp_lcd_new_panel_io_dbi(dsi_bus_, &dbi_config, &dbi_io_);
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }

        // Diagnostic workaround: avoid an infinite ACK/BTA wait when the panel
        // does not acknowledge an initialization command.
        mipi_dsi_host_ll_enable_cmd_ack(MIPI_DSI_LL_GET_HOST(kDsiBusId), false);
        ESP_LOGW(kTag, "MIPI DSI command ACK disabled for diagnosis");

        esp_lcd_dpi_panel_config_t dpi_config{};
        dpi_config.virtual_channel = 0;
        dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
        dpi_config.dpi_clock_freq_mhz = hardware.pixel_clock_mhz;
        dpi_config.in_color_format = config.in_color_format;
        dpi_config.out_color_format = out_color_format;
        dpi_config.num_fbs = config.frame_buffer_count;
        dpi_config.video_timing = hardware.video_timing;
#if T_PANEL_P4_BSP_IS_STANDARD
        pdl::display::Jd9365Config panel_config{};
#elif T_PANEL_P4_BSP_IS_ROUND
        pdl::display::Jd9365daConfig panel_config{};
#else
        pdl::display::Ili9882Config panel_config{};
#endif
        panel_config.dsi_bus = dsi_bus_;
        panel_config.dpi_config = &dpi_config;
#if T_PANEL_P4_BSP_IS_STANDARD
        panel_config.init_sequence = pdl::display::jd9365BoardInitSequence();
#elif T_PANEL_P4_BSP_IS_ROUND
        panel_config.init_sequence = pdl::display::jd9365daBoeRoundInitSequence();
#endif
        panel_config.reset_gpio = GPIO_NUM_NC;
#if T_PANEL_P4_BSP_IS_ROUND
        // The BOE round module needs BGR panel order for RGB framebuffer data.
        panel_config.color_order = LCD_RGB_ELEMENT_ORDER_BGR;
#else
        panel_config.color_order = LCD_RGB_ELEMENT_ORDER_RGB;
#endif
        panel_config.lane_count = kDsiLaneCount;
        panel_config.bits_per_pixel = config.bits_per_pixel;
        panel_config.read_display_id = config.read_display_id;
#if T_PANEL_P4_BSP_IS_STANDARD
        error = pdl::display::newJd9365Panel(dbi_io_, panel_config, &panel_);
#elif T_PANEL_P4_BSP_IS_ROUND
        error = pdl::display::newJd9365daPanel(dbi_io_, panel_config, &panel_);
#else
        error = pdl::display::newIli9882Panel(dbi_io_, panel_config, &panel_);
#endif
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }

        switch (config.frame_buffer_count)
        {
        case 1:
            error = esp_lcd_dpi_panel_get_frame_buffer(panel_, 1, &frame_buffers_[0]);
            break;
        case 2:
            error = esp_lcd_dpi_panel_get_frame_buffer(panel_, 2, &frame_buffers_[0],
                                                       &frame_buffers_[1]);
            break;
        case 3:
            error = esp_lcd_dpi_panel_get_frame_buffer(panel_, 3, &frame_buffers_[0],
                                                       &frame_buffers_[1], &frame_buffers_[2]);
            break;
        default:
            error = ESP_ERR_INVALID_ARG;
            break;
        }
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }
        frame_buffer_count_ = config.frame_buffer_count;

        error = releaseDisplayReset();
        if (error == ESP_OK)
            error = esp_lcd_panel_init(panel_);
        if (error == ESP_OK)
        {
            panel_initialized_ = true;
            error = esp_lcd_panel_disp_on_off(panel_, true);
        }
        if (error != ESP_OK)
        {
            (void)deinitializeDisplay();
            return error;
        }

        display_config_ = config;
        display_initialized_ = true;
        ESP_LOGI(kTag,
                 "%s initialized: %dx%d input %s, panel %u bpp, %u frame buffer(s)",
                 hardware.panel_name, displayWidth, displayHeight,
                 colorFormatName(config.in_color_format), config.bits_per_pixel,
                 frame_buffer_count_);
        return ESP_OK;
    }

    esp_err_t Board::deinitializeDisplay()
    {
        esp_err_t first_error = ESP_OK;

        // Keep the backlight physically off throughout panel/DPI teardown.
        preserveFirstError(deinitializeBacklight(), first_error);

        if (panel_ != nullptr && panel_initialized_)
        {
            preserveFirstError(esp_lcd_panel_disp_on_off(panel_, false), first_error);
        }

        const bool display_resources_created = panel_ != nullptr || dbi_io_ != nullptr ||
                                               dsi_bus_ != nullptr || mipi_phy_ldo_ != nullptr ||
                                               frame_buffer_count_ > 0U || panel_initialized_ ||
                                               display_initialized_;
        if (panel_ != nullptr)
        {
            const esp_err_t panel_error = esp_lcd_panel_del(panel_);
            preserveFirstError(panel_error, first_error);
            // A failed DMA stop leaves a live panel. Its bus and buffers must
            // stay allocated so a failed sleep attempt cannot reuse them.
            if (panel_error != ESP_OK)
            {
                display_initialized_ = false;
                return first_error;
            }
            panel_ = nullptr;
        }
#if !T_PANEL_P4_BSP_IS_RECT
        if (display_resources_created && xl9555_ && xl9555_->isInitialized())
#else
        if (display_resources_created)
#endif
        {
            preserveFirstError(assertDisplayReset(), first_error);
        }
        if (dbi_io_ != nullptr)
        {
            preserveFirstError(esp_lcd_panel_io_del(dbi_io_), first_error);
            dbi_io_ = nullptr;
        }
        if (dsi_bus_ != nullptr)
        {
            preserveFirstError(esp_lcd_del_dsi_bus(dsi_bus_), first_error);
            dsi_bus_ = nullptr;
        }
        if (mipi_phy_ldo_ != nullptr)
        {
            preserveFirstError(esp_ldo_release_channel(mipi_phy_ldo_), first_error);
            mipi_phy_ldo_ = nullptr;
        }

        frame_buffers_.fill(nullptr);
        frame_buffer_count_ = 0;
        panel_initialized_ = false;
        display_initialized_ = false;
        display_config_ = DisplayConfig{};
#if T_PANEL_P4_BSP_IS_STANDARD
        display_sleeping_ = false;
        display_sleep_brightness_ = 0;
#endif
        return first_error;
    }

    bool Board::isDisplayInitialized() const noexcept
    {
        return display_initialized_;
    }

    DisplayConfig Board::displayConfig() const noexcept
    {
        return display_config_;
    }

    esp_lcd_panel_handle_t Board::displayPanel() const noexcept
    {
        return display_initialized_ ? panel_ : nullptr;
    }

    void *Board::frameBuffer(std::uint8_t index) const noexcept
    {
        if (!display_initialized_ || index >= frame_buffer_count_)
            return nullptr;
        return frame_buffers_[index];
    }

    std::uint8_t Board::frameBufferCount() const noexcept
    {
        return display_initialized_ ? frame_buffer_count_ : 0;
    }
}
