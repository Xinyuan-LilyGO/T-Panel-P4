# Jadard touch example

This example uses the `t_panel_p4_standard` BSP to initialize the board I2C
bus, XL9555, the active-low touch reset on XL9555 P0_4, and the Jadard touch
controller at address `0x68`.

GPIO6 is configured as the active-low touch interrupt. The ISR only notifies
the main task; all I2C reads run in task context. At startup the example prints
the chip ID, firmware version, and reported resolution. Touch interrupts then
print up to five coordinates, pressure values, and edge events.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 touch/jadard build
.\build.ps1 touch/jadard flash-monitor COM198
```

The isolated build output is stored in `build/touch/jadard`.
