#include <cstdint>
#include <cstdio>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/board.hpp>

#include <pdl/drivers/charge/axp517/axp517.hpp>

namespace
{
    using PowerConfig = bsp::T_PANEL_P4_BSP_NAMESPACE::PowerConfig;

    // Edit these settings for the connected battery and power source. The BSP
    // applies the same configuration on standard, round, and rect hardware.
    PowerConfig examplePowerConfig()
    {
        PowerConfig config{};
        config.input_current_limit_ma = 1500;
        config.input_voltage_limit_mv = 4600;
        config.minimum_system_voltage_mv = 3500;
        config.charge_voltage_mv = 4200;
        config.charge_current_ma = 512;
        config.precharge_current_ma = 128;
        config.trickle_current_ma = 64;
        config.termination_current_ma = 128;
        config.enable_charge_termination = true;
        config.enable_charger = true;
        config.enable_adc = true;
        config.enable_fuel_gauge = true;
        config.enable_battery_detection = true;
        config.enable_bc12_detection = true;
        config.enable_type_c_detection = true;
        config.enable_charge_led = true;
        config.charge_led_output_type = pdl::charge::Axp517ChargeLedOutputType::open_drain;
        config.charge_led_mode = pdl::charge::Axp517ChargeLedMode::type_a;
        config.charge_led_cfg_state = pdl::charge::Axp517ChargeLedCfgState::high_impedance;
        config.enable_interrupt = false;
        return config;
    }

    esp_err_t printConfiguration(bsp::Board &board, const PowerConfig &config)
    {
        // These are the requested values; the driver quantizes currents and
        // voltages to register steps. The example values use exact steps.
        std::printf("AXP517 configured: input=%u mA / %u mV, minimum-system=%u mV\n",
                    config.input_current_limit_ma, config.input_voltage_limit_mv,
                    config.minimum_system_voltage_mv);
        std::printf("       charge=%u mV / %u mA, precharge=%u mA, trickle=%u mA, termination=%u mA\n",
                    config.charge_voltage_mv, config.charge_current_ma,
                    config.precharge_current_ma, config.trickle_current_ma,
                    config.termination_current_ma);

        pdl::charge::Axp517ChargeLedConfig led{};
        const esp_err_t error = board.readChargeLedConfig(led);
        if (error != ESP_OK)
        {
            std::printf("AXP517 CHGLED readback failed: %s\n", esp_err_to_name(error));
            return error;
        }
        std::printf("       CHGLED readback: enabled=%s, output=%s, mode=%u, cfg-state=%u, REG30=0x%02X\n",
                    led.enabled ? "yes" : "no",
                    led.output_type == pdl::charge::Axp517ChargeLedOutputType::open_drain
                        ? "open-drain" : "push-pull",
                    static_cast<unsigned>(led.mode), static_cast<unsigned>(led.cfg_state),
                    static_cast<unsigned>(led.raw_config));
        std::printf("       BSP interrupts=%s\n", board.isPowerInterruptEnabled() ? "enabled" : "disabled");
        return ESP_OK;
    }

    const char *chargeState(pdl::charge::Axp517ChargeState state)
    {
        switch (state)
        {
        case pdl::charge::Axp517ChargeState::trickle:
            return "trickle";
        case pdl::charge::Axp517ChargeState::precharge:
            return "precharge";
        case pdl::charge::Axp517ChargeState::constant_current:
            return "constant-current";
        case pdl::charge::Axp517ChargeState::constant_voltage:
            return "constant-voltage";
        case pdl::charge::Axp517ChargeState::complete:
            return "complete";
        case pdl::charge::Axp517ChargeState::not_charging:
            return "not-charging";
        default:
            return "unknown";
        }
    }

    const char *currentDirection(pdl::charge::Axp517BatteryCurrentDirection direction)
    {
        switch (direction)
        {
        case pdl::charge::Axp517BatteryCurrentDirection::charging:
            return "charging";
        case pdl::charge::Axp517BatteryCurrentDirection::discharging:
            return "discharging";
        case pdl::charge::Axp517BatteryCurrentDirection::standby:
            return "standby";
        default:
            return "unknown";
        }
    }

    void printPower(pdl::charge::Axp517 &device)
    {
        pdl::charge::Axp517PowerStatus power{};
        pdl::charge::Axp517ModuleEnableStatus modules{};
        std::uint8_t battery_percent{};
        std::uint16_t battery_mv{};
        std::int16_t battery_ma{};
        std::uint16_t vbus_mv{};
        bool charger_enabled{};

        const auto power_error = device.readPowerStatus(power);
        const auto module_error = device.readModuleEnableStatus(modules);
        const auto charger_error = device.readChargerEnabled(charger_enabled);
        const auto percent_error = device.readBatteryPercentage(battery_percent);
        const auto voltage_error = device.readBatteryVoltageMv(battery_mv);
        const auto current_error = device.readBatteryCurrentMa(battery_ma);
        const auto vbus_error = device.readVbusVoltageMv(vbus_mv);

        std::printf("\n[AXP517] ");
        if (power_error == pdl::Status::ok)
        {
            std::printf("VBUS=%s, battery=%s, charging=%s, direction=%s, charge-state=%s",
                        power.vbus_present ? "present" : "absent",
                        power.battery_present ? "present" : "absent",
                        pdl::charge::isBatteryCharging(power) ? "yes" : "no",
                        currentDirection(power.battery_current_direction),
                        chargeState(power.charge_state));
        }
        else
        {
            std::printf("power-status=error(%s)", pdl::statusName(power_error));
        }

        if (module_error == pdl::Status::ok)
        {
            std::printf(", boost=%s, buck=%s, charger=%s",
                        modules.boost_enabled ? "enabled" : "disabled",
                        modules.buck_enabled ? "enabled" : "disabled",
                        modules.charger_enabled ? "enabled" : "disabled");
        }
        else
        {
            std::printf(", module-status=error(%s)", pdl::statusName(module_error));
        }

        std::printf("\n       battery: ");
        if (percent_error == pdl::Status::ok && voltage_error == pdl::Status::ok && current_error == pdl::Status::ok)
            std::printf("%u%%, %u mV, %d mA", battery_percent, battery_mv, battery_ma);
        else
            std::printf("read error(percent=%s, voltage=%s, current=%s)",
                        pdl::statusName(percent_error), pdl::statusName(voltage_error),
                        pdl::statusName(current_error));
        if (vbus_error == pdl::Status::ok)
            std::printf(", VBUS=%u mV", vbus_mv);
        else
            std::printf(", VBUS=error(%s)", pdl::statusName(vbus_error));
        if (charger_error == pdl::Status::ok)
            std::printf(", charger-control=%s", charger_enabled ? "enabled" : "disabled");
        else
            std::printf(", charger-control=error(%s)", pdl::statusName(charger_error));
        std::printf("\n");
    }
}

extern "C" void app_main(void)
{
    bsp::Board board;
    const PowerConfig config = examplePowerConfig();
    std::printf("\nAXP517 BSP configuration and power monitor\n");
    const esp_err_t error = board.initializePower(config);
    if (error != ESP_OK)
    {
        std::printf("BSP initialization failed: %s\n", esp_err_to_name(error));
        return;
    }

    if (printConfiguration(board, config) != ESP_OK)
    {
        (void)board.deinitialize();
        return;
    }

    // initializePower() configured the BSP-owned AXP517, including ADC and
    // fuel gauge. Borrow that driver instead of creating or initializing one.
    auto bsp_device = board.devices();
    auto *axp517 = bsp_device.axp517;
    if (axp517 == nullptr)
    {
        std::printf("BSP AXP517 device is not initialized\n");
        (void)board.deinitialize();
        return;
    }

    std::uint16_t vendor_id{};
    const pdl::Status id_status = axp517->readId(vendor_id);
    if (pdl::isOk(id_status))
        std::printf("AXP517 TCPC vendor ID: 0x%04X\n", static_cast<unsigned>(vendor_id));
    else
        std::printf("AXP517 vendor ID read failed: %s\n", pdl::statusName(id_status));

    // Allow the newly enabled ADC and fuel gauge to produce measurements.
    vTaskDelay(pdMS_TO_TICKS(100));
    while (true)
    {
        printPower(*axp517);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
