# AXP517 hardware test

This example uses the real `peripheral_drivers_lib` ESP-IDF I2C transport to
test the AXP517 connected to SDA GPIO3 and SCL GPIO2 at address `0x34`.

The test checks communication, validates the device ID, enables the ADC and
battery gauge, reads power and fault status, and samples available telemetry.
Battery and VBUS measurements are skipped when the corresponding source is not
present. It does not change charging voltage/current or enter ship mode.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 axp517 flash-monitor COM198
```

The build output is stored in `build/axp517`.
