# LVGL LCD and touch example

This example integrates LVGL 9.5.0 with the shared T-Panel-P4 BSP on all three
hardware profiles: standard/round at 720x720 and rect at 720x1440. The BSP
selects the display and touch chips. The first active touch
point is reported through an LVGL pointer input device, and its X/Y coordinates
are shown in the upper-left corner of the display.

Two 48-line RGB565 LVGL draw buffers are allocated from PSRAM. The BSP keeps
ownership of the DPI framebuffer, display panel, I2C bus, touch reset, and
touch controller.

The display input format is explicitly RGB565; the panel output remains
24-bit. Touch reads use `bsp::TouchData`; `busy` preserves the previous input
state until a fresh report arrives.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 -HardwareVersion standard -Example lvgl_lcd_touch -Action build
.\build.ps1 -HardwareVersion standard -Example lvgl_lcd_touch -Action flash-monitor -Port COM198
```

Select `round` or `rect` for the corresponding board. The isolated build output
is stored in `build/<hardware>-lvgl_lcd_touch`.
