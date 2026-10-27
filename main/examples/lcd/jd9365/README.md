# JD9365 color-bar example

This example uses the `t_panel_p4_standard` BSP to initialize the IVO
5.0-inch JD9365TX panel as a 720x720 RGB565 MIPI-DSI display and shows five
vertical bars: red, green, blue, white, and black.

Board connections come from the BSP's
`bsp/t_panel_p4_standard_pins.hpp` header:

- I2C SDA/SCL: GPIO3/GPIO2
- XL9555 address: `0x20`
- LCD reset: XL9555 P0_3, active low
- LCD backlight: GPIO34, active high

The BSP owns the I2C bus, XL9555, reset sequence, backlight PWM, MIPI PHY LDO,
DSI/DBI handles, panel, and framebuffer. The MIPI DSI PHY uses internal LDO
channel 3 at 2.5 V. The panel uses two data lanes at 500 Mbps and a 40 MHz DPI
pixel clock. The full-screen RGB565 framebuffer is allocated from PSRAM.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 lcd_jd9365 build
.\build.ps1 lcd_jd9365 flash-monitor COM198
```

The isolated build output is stored in `build/lcd_jd9365`.
