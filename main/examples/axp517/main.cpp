#include <cstdint>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <pdl/core/status.hpp>
#include <pdl/drivers/charge/axp517/axp517.hpp>
#include <pdl/ports/esp_idf/i2c_bus.hpp>
#include <pdl/ports/esp_idf/i2c_device.hpp>

namespace
{
    constexpr gpio_num_t kI2cSda = GPIO_NUM_3;
    constexpr gpio_num_t kI2cScl = GPIO_NUM_2;
    constexpr std::uint32_t kI2cFrequencyHz = 400000;

    class TestResults
    {
    public:
        bool check(const char *name, pdl::Status status)
        {
            if (pdl::isOk(status))
            {
                ++passed_;
                std::printf("[PASS] %s\n", name);
                return true;
            }

            ++failed_;
            std::printf("[FAIL] %s: %s\n", name, pdl::statusName(status));
            return false;
        }

        bool checkValue(const char *name, bool valid)
        {
            if (valid)
            {
                ++passed_;
                std::printf("[PASS] %s\n", name);
                return true;
            }

            ++failed_;
            std::printf("[FAIL] %s\n", name);
            return false;
        }

        void skip(const char *name, const char *reason)
        {
            ++skipped_;
            std::printf("[SKIP] %s: %s\n", name, reason);
        }

        void printSummary() const
        {
            std::printf(
                "\nAXP517 test result: %s (%u passed, %u failed, %u skipped)\n",
                failed_ == 0U ? "PASS" : "FAIL",
                passed_,
                failed_,
                skipped_);
        }

    private:
        unsigned int passed_{};
        unsigned int failed_{};
        unsigned int skipped_{};
    };

    const char *yesNo(bool value)
    {
        return value ? "yes" : "no";
    }

    void testBatteryMeasurements(pdl::charge::Axp517 &axp517, const pdl::charge::Axp517PowerStatus &power, TestResults &results)
    {
        if (!power.battery_present)
        {
            results.skip("battery measurements", "battery is not present");
            return;
        }

        std::uint8_t battery_percent{};
        if (results.check(
                "read battery percentage",
                axp517.readBatteryPercentage(battery_percent)))
        {
            std::printf("       Battery: %u %%\n", battery_percent);
        }

        std::uint8_t state_of_health{};
        if (results.check(
                "read battery state of health",
                axp517.readBatteryStateOfHealth(state_of_health)))
        {
            std::printf("       State of health: %u %%\n", state_of_health);
        }

        std::uint16_t battery_mv{};
        if (results.check(
                "read battery voltage",
                axp517.readBatteryVoltageMv(battery_mv)))
        {
            std::printf("       Battery voltage: %u mV\n", battery_mv);
        }

        std::int16_t battery_ma{};
        if (results.check(
                "read battery current",
                axp517.readBatteryCurrentMa(battery_ma)))
        {
            std::printf("       Battery current: %d mA\n", battery_ma);
        }

        std::uint16_t ts_mv{};
        if (results.check("read TS voltage", axp517.readTsVoltageMv(ts_mv)))
        {
            std::printf("       TS voltage: %u mV\n", ts_mv);
        }
    }

    void testVbusMeasurements(pdl::charge::Axp517 &axp517, const pdl::charge::Axp517PowerStatus &power, TestResults &results)
    {
        if (!power.vbus_present)
        {
            results.skip("VBUS measurements", "VBUS is not present");
            return;
        }

        std::uint16_t vbus_mv{};
        if (results.check("read VBUS voltage", axp517.readVbusVoltageMv(vbus_mv)))
        {
            std::printf("       VBUS voltage: %u mV\n", vbus_mv);
        }

        std::uint16_t input_ma{};
        if (results.check("read input current", axp517.readInputCurrentMa(input_ma)))
        {
            std::printf("       Input current: %u mA\n", input_ma);
        }
    }

    void runAxp517Test()
    {
        TestResults results;
        pdl::esp_idf::I2cBus i2c_bus;

        std::printf("\nAXP517 hardware test\n");
        std::printf(
            "I2C: SDA=GPIO%d, SCL=GPIO%d, frequency=%lu Hz, address=0x%02X\n\n",
            static_cast<int>(kI2cSda),
            static_cast<int>(kI2cScl),
            static_cast<unsigned long>(kI2cFrequencyHz),
            pdl::charge::Axp517::defaultI2cAddress);

        if (!results.check("initialize I2C bus", i2c_bus.initialize(kI2cSda, kI2cScl)))
        {
            results.printSummary();
            return;
        }

        pdl::esp_idf::I2cDevice transport(
            i2c_bus,
            pdl::charge::Axp517::defaultI2cAddress,
            kI2cFrequencyHz);
        pdl::charge::Axp517 axp517(transport);

        if (!results.check("initialize AXP517", axp517.initialize()))
        {
            results.check("deinitialize I2C bus", i2c_bus.deinitialize());
            results.printSummary();
            return;
        }

        std::uint16_t device_id{};
        if (results.check("read AXP517 device ID", axp517.readId(device_id)))
        {
            std::printf("       Device ID: 0x%04X\n", device_id);
            results.checkValue(
                "validate AXP517 device ID",
                device_id == 0x1F3AU || device_id == 0xE0C5U);
        }

        results.check("enable ADC channels", axp517.setAdcChannels(pdl::charge::Axp517::adcAll));
        results.check("enable battery detection", axp517.setBatteryDetectionEnabled(true));
        results.check("enable fuel gauge", axp517.setFuelGaugeEnabled(true));
        vTaskDelay(pdMS_TO_TICKS(100));

        pdl::charge::Axp517PowerStatus power{};
        const bool power_valid = results.check("read power status", axp517.readPowerStatus(power));
        if (power_valid)
        {
            std::printf(
                "       VBUS=%s, battery=%s, system=%s, BATFET=%s\n",
                yesNo(power.vbus_present),
                yesNo(power.battery_present),
                yesNo(power.system_on),
                yesNo(power.batfet_on));
            std::printf(
                "       voltage_limited=%s, current_limited=%s, thermal_regulation=%s\n",
                yesNo(power.input_voltage_limited),
                yesNo(power.input_current_limited),
                yesNo(power.thermal_regulation));
        }

        pdl::charge::Axp517FaultStatus fault{};
        if (results.check("read fault status", axp517.readFaultStatus(fault)))
        {
            std::printf(
                "       Fault registers: 0x%02X 0x%02X, system_OV=%s, battery_UV=%s\n",
                fault.raw_fault0,
                fault.raw_fault1,
                yesNo(fault.system_over_voltage),
                yesNo(fault.battery_under_voltage));
        }

        pdl::charge::Axp517UsbChargerType charger_type{};
        if (results.check("read USB charger type", axp517.readUsbChargerType(charger_type)))
        {
            std::printf("       USB charger type code: %u\n", static_cast<unsigned int>(charger_type));
        }

        std::uint16_t system_mv{};
        if (results.check("read system voltage", axp517.readSystemVoltageMv(system_mv)))
        {
            std::printf("       System voltage: %u mV\n", system_mv);
        }

        float die_temperature_c{};
        if (results.check(
                "read die temperature",
                axp517.readDieTemperatureC(die_temperature_c)))
        {
            std::printf("       Die temperature: %.1f C\n", static_cast<double>(die_temperature_c));
        }

        std::uint32_t interrupts{};
        if (results.check("read interrupt status", axp517.readInterruptStatus(interrupts)))
        {
            std::printf("       Interrupt status: 0x%08lX\n", static_cast<unsigned long>(interrupts));
        }

        if (power_valid)
        {
            testBatteryMeasurements(axp517, power, results);
            testVbusMeasurements(axp517, power, results);
        }
        else
        {
            results.skip("battery measurements", "power status is unavailable");
            results.skip("VBUS measurements", "power status is unavailable");
        }

        results.check("deinitialize AXP517", axp517.deinitialize());
        results.check("deinitialize I2C bus", i2c_bus.deinitialize());
        results.printSummary();
    }
}

extern "C" void app_main(void)
{
    runAxp517Test();
}
