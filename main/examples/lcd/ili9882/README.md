# ILI9882 RECT color-bar example

This example builds the RECT BSP and initializes the HD0549S001 ILI9882 panel
through `peripheral_drivers_lib`. It allocates a RGB565 framebuffer, draws
five vertical color bars across the 720x1440 panel, and enables the direct
GPIO23 backlight.

Build with the isolated RECT example variant:

```powershell
.\build.ps1 lcd/ili9882 build
```

The output directory is `build/lcd/ili9882`.
