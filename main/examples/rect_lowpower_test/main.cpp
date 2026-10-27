#include <array>
#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_check.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_rect.hpp>
#include <pdl/core/status.hpp>
#include <pdl/drivers/audio/es7210/es7210.hpp>
#include <pdl/drivers/motor/drv2605/drv2605.hpp>
#include <pdl/drivers/sensor/qmc6309/qmc6309.hpp>
#include <pdl/drivers/sensor/qmi8658c/qmi8658c.hpp>
#include <pdl/ports/esp_idf/i2c_device.hpp>
#include <pdl/ports/esp_idf/task_delay.hpp>
#include <pdl/drivers/display/ili9882/ili9882.hpp>
#include <pdl/drivers/touch/ili9882/ili9882.hpp>

namespace
{
    constexpr char kTag[] = "lcd_ili9882";
    constexpr std::array<std::uint16_t, 5> kRgb565Bars{
        0xF800,
        0x07E0,
        0x001F,
        0xFFFF,
        0x0000,
    };

    bsp::t_panel_p4_rect::Board board;

    esp_err_t drawColorBars()
    {
        auto *pixels = static_cast<std::uint16_t *>(board.frameBuffer());
        ESP_RETURN_ON_FALSE(
            pixels != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "display framebuffer is unavailable");

        constexpr int width = bsp::t_panel_p4_rect::Board::displayWidth;
        constexpr int height = bsp::t_panel_p4_rect::Board::displayHeight;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::size_t bar =
                    static_cast<std::size_t>(x * kRgb565Bars.size() / width);
                pixels[y * width + x] = kRgb565Bars[bar];
            }
        }

        return esp_lcd_panel_draw_bitmap(
            board.displayPanel(), 0, 0, width, height, pixels);
    }

    void deinitializeAdditionalDevices(
        pdl::audio::Es7210 &es7210,
        pdl::motor::Drv2605 &drv2605,
        pdl::sensor::Qmc6309 &qmc6309,
        pdl::sensor::Qmi8658c &qmi8658c)
    {
        const pdl::Status es7210_status = es7210.deinitialize();
        if (pdl::isError(es7210_status))
            ESP_LOGW(kTag, "ES7210 deinitialization failed: %s", pdl::statusName(es7210_status));
        const pdl::Status drv2605_status = drv2605.deinitialize();
        if (pdl::isError(drv2605_status))
            ESP_LOGW(kTag, "DRV2605 deinitialization failed: %s", pdl::statusName(drv2605_status));
        const pdl::Status qmc6309_status = qmc6309.deinitialize();
        if (pdl::isError(qmc6309_status))
            ESP_LOGW(kTag, "QMC6309 deinitialization failed: %s", pdl::statusName(qmc6309_status));
        const pdl::Status qmi8658c_status = qmi8658c.deinitialize();
        if (pdl::isError(qmi8658c_status))
            ESP_LOGW(kTag, "QMI8658C deinitialization failed: %s", pdl::statusName(qmi8658c_status));
    }

    esp_err_t enterPanelAndTouchSleep(
        pdl::audio::Es7210 &es7210,
        pdl::motor::Drv2605 &drv2605,
        pdl::sensor::Qmc6309 &qmc6309,
        pdl::sensor::Qmi8658c &qmi8658c)
    {
        auto *touch = board.touchController();
        ESP_RETURN_ON_FALSE(
            touch != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "ILI9882 touch controller is unavailable");

        const pdl::Status touch_status = touch->setState(
            pdl::touch::Ili9882State::sleeping);
        ESP_RETURN_ON_FALSE(
            pdl::isOk(touch_status),
            ESP_FAIL,
            kTag,
            "enter ILI9882 touch sleep failed: %s",
            pdl::statusName(touch_status));
        vTaskDelay(pdMS_TO_TICKS(1000));

        board.deinitializeCamera();
        vTaskDelay(pdMS_TO_TICKS(1000));
        board.deinitializeAudio();
        vTaskDelay(pdMS_TO_TICKS(1000));
        board.deinitializeSdCard();
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_RETURN_ON_ERROR(
            pdl::display::setPanelState(
                board.displayPanel(),
                pdl::display::Ili9882State::sleeping),
            kTag,
            "enter ILI9882 display sleep failed");
        ESP_RETURN_ON_ERROR(
            board.setBacklightBrightness(0),
            kTag,
            "turn off backlight failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        board.deinitializeDisplay();
        vTaskDelay(pdMS_TO_TICKS(1000));
        board.setDisplayPowerEnabled(false);
        vTaskDelay(pdMS_TO_TICKS(1000));

        board.setSpeakerEnabled(false);
        vTaskDelay(pdMS_TO_TICKS(1000));

        // All devices attached to the shared I2C bus must be released before
        // Board::enterDeepSleep() tears the bus down.
        deinitializeAdditionalDevices(es7210, drv2605, qmc6309, qmi8658c);
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI(kTag, "ILI9882 display and touch entered sleep mode");
        bsp::t_panel_p4_rect::DeepSleepConfig deep_sleep_config{};
        deep_sleep_config.wakeup_gpio = bsp::t_panel_p4_rect::pins::button;
        deep_sleep_config.wakeup_on_high = false;
        ESP_RETURN_ON_ERROR(
            board.enterDeepSleep(deep_sleep_config),
            kTag,
            "enter ESP32-P4 deep sleep failed");
        return ESP_OK;
    }

    esp_err_t runExample()
    {
        ESP_RETURN_ON_ERROR(board.initialize(), kTag, "RECT BSP initialization failed");

        // The BSP owns the shared-bus ES8389, SGM38121 and OV2710 lifecycles.
        ESP_RETURN_ON_ERROR(
            board.initializeAudio(),
            kTag,
            "ES8389 initialization failed");
        ESP_RETURN_ON_ERROR(
            board.initializeCamera(),
            kTag,
            "SGM38121/OV2710 initialization failed");

        ESP_RETURN_ON_ERROR(
            board.initializeSdCard(),
            kTag,
            "SD card initialization failed");

        auto *i2c_bus = board.i2cBus();
        ESP_RETURN_ON_FALSE(
            i2c_bus != nullptr,
            ESP_ERR_INVALID_STATE,
            kTag,
            "RECT shared I2C bus is unavailable");

        // The remaining devices are initialized directly on the same bus.
        pdl::esp_idf::TaskDelay task_delay;
        pdl::esp_idf::I2cDevice es7210_transport(
            *i2c_bus,
            pdl::audio::Es7210::defaultI2cAddress,
            pdl::audio::Es7210::defaultI2cFrequencyHz);
        pdl::audio::Es7210 es7210(es7210_transport);
        const pdl::Status es7210_status = es7210.initialize();
        ESP_RETURN_ON_FALSE(
            pdl::isOk(es7210_status),
            ESP_FAIL,
            kTag,
            "ES7210 initialization failed: %s",
            pdl::statusName(es7210_status));

        pdl::esp_idf::I2cDevice drv2605_transport(
            *i2c_bus,
            pdl::motor::Drv2605::defaultI2cAddress,
            pdl::motor::Drv2605::defaultI2cFrequencyHz);
        pdl::motor::Drv2605 drv2605(drv2605_transport);
        const pdl::Status drv2605_status = drv2605.initialize();
        ESP_RETURN_ON_FALSE(
            pdl::isOk(drv2605_status),
            ESP_FAIL,
            kTag,
            "DRV2605 initialization failed: %s",
            pdl::statusName(drv2605_status));

        pdl::esp_idf::I2cDevice qmc6309_transport(
            *i2c_bus,
            pdl::sensor::Qmc6309::defaultI2cAddress,
            pdl::sensor::Qmc6309::defaultI2cFrequencyHz);
        pdl::sensor::Qmc6309 qmc6309(qmc6309_transport, task_delay);
        const pdl::Status qmc6309_status = qmc6309.initialize();
        ESP_RETURN_ON_FALSE(
            pdl::isOk(qmc6309_status),
            ESP_FAIL,
            kTag,
            "QMC6309 initialization failed: %s",
            pdl::statusName(qmc6309_status));

        pdl::esp_idf::I2cDevice qmi8658c_transport(
            *i2c_bus,
            // The RECT board straps QMI8658C SA0 low (7-bit address 0x6A).
            pdl::sensor::Qmi8658c::lowI2cAddress,
            pdl::sensor::Qmi8658c::defaultI2cFrequencyHz);
        pdl::sensor::Qmi8658c qmi8658c(qmi8658c_transport, task_delay);
        const pdl::Status qmi8658c_status = qmi8658c.initialize();
        ESP_RETURN_ON_FALSE(
            pdl::isOk(qmi8658c_status),
            ESP_FAIL,
            kTag,
            "QMI8658C initialization failed: %s",
            pdl::statusName(qmi8658c_status));

        ESP_LOGI(
            kTag,
            "ES7210, ES8389, SGM38121, OV2710, DRV2605, QMC6309 and QMI8658C initialized");
        ESP_RETURN_ON_ERROR(
            board.initializeDisplay(),
            kTag,
            "ILI9882 display initialization failed");
        bsp::t_panel_p4_rect::TouchConfig touch_config{};
        touch_config.perform_software_reset = false;
        ESP_RETURN_ON_ERROR(
            board.initializeTouch(touch_config),
            kTag,
            "ILI9882 touch initialization failed");
        ESP_RETURN_ON_ERROR(drawColorBars(), kTag, "draw color bars failed");
        ESP_RETURN_ON_ERROR(
            board.setBacklightBrightness(100),
            kTag,
            "enable backlight failed");

        // ESP_LOGI(kTag, "ILI9882 720x1440 color bars ready through RECT BSP");
        vTaskDelay(pdMS_TO_TICKS(5000));
        return enterPanelAndTouchSleep(es7210, drv2605, qmc6309, qmi8658c);
    }
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(runExample());
    while (true)
        vTaskDelay(portMAX_DELAY);
}
