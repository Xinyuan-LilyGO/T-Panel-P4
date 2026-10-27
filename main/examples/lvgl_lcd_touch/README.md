# LVGL LCD and touch example

This example integrates LVGL 9.5.0 with the T-Panel-P4 Standard 720x720
JD9365 MIPI-DSI display and Jadard touch controller. The first active touch
point is reported through an LVGL pointer input device, and its X/Y coordinates
are shown in the upper-left corner of the display.

Two 48-line RGB565 LVGL draw buffers are allocated from PSRAM. The BSP keeps
ownership of the DPI framebuffer, display panel, I2C bus, touch reset, and
touch controller.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 lvgl_lcd_touch build
.\build.ps1 lvgl_lcd_touch flash-monitor COM198
```

The isolated build output is stored in `build/lvgl_lcd_touch`.
