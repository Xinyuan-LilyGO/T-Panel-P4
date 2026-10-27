#include <bsp/t_panel_p4_round.hpp>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/mipi_dsi_ll.h>

#include <pdl/drivers/display/jd9365/jd9365.hpp>

#include "bsp_private.hpp"

namespace
{
constexpr char kTag[] = "t_panel_p4_display";
constexpr int kDsiBusId = 0;
constexpr std::uint8_t kDsiLaneCount = 2;
constexpr float kDsiLaneBitRateMbps = 500.0F;
constexpr float kPixelClockMhz = 40.0F;
constexpr int kMipiPhyLdoChannel = 3;
constexpr int kMipiPhyVoltageMv = 2500;
constexpr std::uint32_t kDisplayResetReleaseDelayMs = 250;
constexpr ledc_mode_t kBacklightSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_0;
constexpr ledc_timer_bit_t kBacklightResolution = LEDC_TIMER_11_BIT;
constexpr std::uint32_t kBacklightFrequencyHz = 30000;

void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
{
    if (first_error == ESP_OK && candidate != ESP_OK)
        first_error = candidate;
}
}

namespace bsp::t_panel_p4_round
{
esp_err_t Board::initializeBacklight()
{
    if (backlight_initialized_)
        return ESP_OK;

    ESP_RETURN_ON_ERROR(
        gpio_set_direction(pins::lcdBacklightPwm, GPIO_MODE_OUTPUT),
        kTag,
        "configure backlight GPIO failed");
    ESP_RETURN_ON_ERROR(
        gpio_set_level(pins::lcdBacklightPwm, 0),
        kTag,
        "drive backlight low failed");
    ESP_RETURN_ON_ERROR(
        gpio_hold_dis(pins::lcdBacklightPwm),
        kTag,
        "release backlight GPIO hold failed");

    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = kBacklightSpeedMode;
    timer_config.duty_resolution = kBacklightResolution;
    timer_config.timer_num = kBacklightTimer;
    timer_config.freq_hz = kBacklightFrequencyHz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(
        ledc_timer_config(&timer_config),
        kTag,
        "configure backlight timer failed");

    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = pins::lcdBacklightPwm;
    channel_config.speed_mode = kBacklightSpeedMode;
    channel_config.channel = kBacklightChannel;
    channel_config.timer_sel = kBacklightTimer;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    ESP_RETURN_ON_ERROR(
        ledc_channel_config(&channel_config),
        kTag,
        "configure backlight channel failed");

    backlight_initialized_ = true;
    backlight_brightness_ = 0;
    return ESP_OK;
}

esp_err_t Board::setBacklightBrightness(std::uint8_t percent)
{
    if (!backlight_initialized_)
        return ESP_ERR_INVALID_STATE;
    if (percent > 100U)
        return ESP_ERR_INVALID_ARG;

    constexpr std::uint32_t maximum_duty =
        (std::uint32_t{1} << kBacklightResolution) - 1U;
    const std::uint32_t duty = maximum_duty * percent / 100U;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(kBacklightSpeedMode, kBacklightChannel, duty),
        kTag,
        "set backlight duty failed");
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(kBacklightSpeedMode, kBacklightChannel),
        kTag,
        "update backlight duty failed");

    backlight_brightness_ = percent;
    return ESP_OK;
}

std::uint8_t Board::backlightBrightness() const noexcept
{
    return backlight_brightness_;
}

esp_err_t Board::assertDisplayReset()
{
    ESP_RETURN_ON_ERROR(
        setExpanderOutput(pins::lcdReset, false),
        kTag,
        "assert LCD reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t Board::releaseDisplayReset()
{
    ESP_RETURN_ON_ERROR(
        setExpanderOutput(pins::lcdReset, true),
        kTag,
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
    if (!initialized_)
        return ESP_ERR_INVALID_STATE;
    if (display_initialized_)
        return ESP_OK;
    if (config.frame_buffer_count == 0U ||
        config.frame_buffer_count > maximumFrameBufferCount)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(
        setBacklightBrightness(0),
        kTag,
        "turn backlight off failed");
    ESP_RETURN_ON_ERROR(assertDisplayReset(), kTag, "LCD reset failed");

    esp_ldo_channel_config_t ldo_config{};
    ldo_config.chan_id = kMipiPhyLdoChannel;
    ldo_config.voltage_mv = kMipiPhyVoltageMv;
    esp_err_t error = esp_ldo_acquire_channel(&ldo_config, &mipi_phy_ldo_);
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
    bus_config.lane_bit_rate_mbps = kDsiLaneBitRateMbps;
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
    dpi_config.dpi_clock_freq_mhz = kPixelClockMhz;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = config.frame_buffer_count;
    dpi_config.video_timing.h_size = displayWidth;
    dpi_config.video_timing.v_size = displayHeight;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_back_porch = 44;
    dpi_config.video_timing.hsync_front_porch = 46;
    dpi_config.video_timing.vsync_pulse_width = 5;
    dpi_config.video_timing.vsync_back_porch = 16;
    dpi_config.video_timing.vsync_front_porch = 50;

    pdl::display::Jd9365Config panel_config{};
    panel_config.dsi_bus = dsi_bus_;
    panel_config.dpi_config = &dpi_config;
    panel_config.init_sequence = pdl::display::jd9365BoardInitSequence();
    panel_config.reset_gpio = GPIO_NUM_NC;
    panel_config.color_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.lane_count = kDsiLaneCount;
    panel_config.bits_per_pixel = displayBitsPerPixel;
    panel_config.read_display_id = config.read_display_id;
    error = pdl::display::newJd9365Panel(dbi_io_, panel_config, &panel_);
    if (error != ESP_OK)
    {
        (void)deinitializeDisplay();
        return error;
    }

    switch (config.frame_buffer_count)
    {
    case 1:
        error = esp_lcd_dpi_panel_get_frame_buffer(
            panel_, 1, &frame_buffers_[0]);
        break;
    case 2:
        error = esp_lcd_dpi_panel_get_frame_buffer(
            panel_, 2, &frame_buffers_[0], &frame_buffers_[1]);
        break;
    case 3:
        error = esp_lcd_dpi_panel_get_frame_buffer(
            panel_, 3, &frame_buffers_[0], &frame_buffers_[1], &frame_buffers_[2]);
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

    display_initialized_ = true;
    ESP_LOGI(
        kTag,
        "IVO JD9365 initialized: %dx%d RGB565, %u frame buffer(s)",
        displayWidth,
        displayHeight,
        frame_buffer_count_);
    return ESP_OK;
}

esp_err_t Board::deinitializeDisplay()
{
    esp_err_t first_error = ESP_OK;

    if (backlight_initialized_)
        preserveFirstError(setBacklightBrightness(0), first_error);

    if (panel_ != nullptr && panel_initialized_)
    {
        preserveFirstError(
            esp_lcd_panel_disp_on_off(panel_, false),
            first_error);
    }

    const bool display_resources_created =
        panel_ != nullptr || dbi_io_ != nullptr || dsi_bus_ != nullptr ||
        mipi_phy_ldo_ != nullptr || frame_buffer_count_ > 0U ||
        panel_initialized_ || display_initialized_;
    if (display_resources_created && initialized_ && xl9555_ &&
        xl9555_->isInitialized())
    {
        preserveFirstError(assertDisplayReset(), first_error);
    }

    if (panel_ != nullptr)
    {
        preserveFirstError(esp_lcd_panel_del(panel_), first_error);
        panel_ = nullptr;
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
        preserveFirstError(
            esp_ldo_release_channel(mipi_phy_ldo_),
            first_error);
        mipi_phy_ldo_ = nullptr;
    }

    frame_buffers_.fill(nullptr);
    frame_buffer_count_ = 0;
    panel_initialized_ = false;
    display_initialized_ = false;
    return first_error;
}

bool Board::isDisplayInitialized() const noexcept
{
    return display_initialized_;
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
