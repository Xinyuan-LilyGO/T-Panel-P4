# AXP517 BSP configuration and power monitor

This example configures and initializes the selected T-Panel-P4 power service through
its BSP APIs and periodically prints AXP517 battery, charger, VBUS, and Boost status to the
serial console. It supports the `standard`, `round`, and `rect` hardware
variants.

All three hardware variants use the same source code. The selected BSP
provides `<bsp/board.hpp>` and `bsp::Board`, so the example needs no hardware
version conditionals. It obtains the existing AXP517 driver from the BSP
device collection:

```cpp
#include <bsp/board.hpp>

bsp::Board board;
bsp::T_PANEL_P4_BSP_NAMESPACE::PowerConfig config{};
config.input_current_limit_ma = 1500;
config.input_voltage_limit_mv = 4600;
config.charge_voltage_mv = 4200;
config.charge_current_ma = 512;
config.enable_adc = true;
config.enable_fuel_gauge = true;
config.enable_battery_detection = true;
ESP_ERROR_CHECK(board.initializePower(config));

auto bsp_device = board.devices();
auto *axp517 = bsp_device.axp517;
if (axp517 == nullptr)
    return;

std::uint8_t battery_percent{};
const pdl::Status status = axp517->readBatteryPercentage(battery_percent);
if (pdl::isError(status))
    std::printf("Battery read failed: %s\n", pdl::statusName(status));
```

`examplePowerConfig()` in `main.cpp` is the parameter entry point for all three
hardware versions. It explicitly sets the following values before calling
`board.initializePower(config)`:

| Settings | Example values |
| --- | --- |
| Input current / voltage limits | 1500 mA / 4600 mV |
| Minimum system voltage | 3500 mV |
| Charge voltage / current | 4200 mV / 512 mA |
| Precharge / trickle / termination currents | 128 / 64 / 128 mA |
| Charger / charge termination | Enabled |
| ADC / fuel gauge / battery detection | Enabled |
| BC1.2 / Type-C detection | Enabled |
| CHGLED | Enabled, open-drain, Type A, CFG state high-impedance |
| Power interrupts | Disabled (polling) |

Choose charging values that match the connected battery and input source.
The driver validates ranges and quantizes values to register steps; the example
values use exact steps. Startup prints the requested voltage/current settings
and the actual CHGLED configuration read back through the BSP. Module enable
states and charging status are read from the device during each poll.

`board.initializePower(config)` automatically
prepares I2C and, on boards that have it, XL9555. Display, audio, camera, LoRa,
and SD-card initialization are unnecessary for this power monitor. The
example reads the TCPC vendor ID once, waits 100 ms for measurements, then polls
the driver every two seconds.
Chip operations return `pdl::Status`; BSP initialization returns `esp_err_t`.
Edit `examplePowerConfig()` to change the startup settings. Calling
`initializePower(config)` again reuses the initialized controller and preserves
its existing settings; call `deinitializePower()` before initializing with a new
configuration, then reacquire `board.devices()`. For runtime changes, common
Board APIs include `setChargerEnabled()`, `setChargeLedEnabled()`, `setChargeLedOutputType()`,
`setChargeLedMode()`, `setChargeLedCfgState()`, and `readChargeLedConfig()`.
`setPowerInterruptEnabled(true/false)` switches interrupts at runtime, including
after initialization with interrupts disabled; `isPowerInterruptEnabled()` reports
the BSP switch state. Enabling uses the configured mask and clears stale events.
`deinitializePower()` disables ADC, fuel gauge, battery, BC1.2, and Type-C
detection before releasing resources. See the [BSP power configuration](../../../components/bsp/t_panel_p4/README.md#电源配置)
for configuration fields and callback lifecycle rules.
The Board owns the driver and its transport. Use Board APIs for initialization
and teardown, and reacquire the device collection after lifecycle changes.

Build and monitor the round example with:

```powershell
.\build.ps1 -Example axp517 -HardwareVersion round -Action flash-monitor -Port COM198
```

Select `standard` or `rect` with the same command's `-HardwareVersion` option:

```powershell
.\build.ps1 axp517 -HardwareVersion standard -Action flash-monitor -Port COM198
.\build.ps1 axp517 -HardwareVersion rect -Action flash-monitor -Port COM198
```
