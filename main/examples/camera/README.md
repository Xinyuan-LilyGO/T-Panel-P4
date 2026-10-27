# OV2710 camera preview

The T-Panel-P4 Standard BSP owns the complete camera lifecycle: SGM38121
power sequencing, OV2710 configuration, MIPI CSI and ISP setup, RGB565 PSRAM
capture buffers, frame synchronization, streaming, and cleanup. Its default
camera configuration captures 1920x1080 RAW10 and converts it to RGB565.

This example only initializes the display and uses PPA to center-crop the BSP
camera frame to 960x960, then scales it to a 720x720 JD9365 preview. The BSP's
two default RGB565 capture buffers use about 7.9 MiB of PSRAM.

Build it in a dedicated directory:

```powershell
idf.py -B build/camera -DT_PANEL_P4_EXAMPLE=camera build
```

The serial log reports received frames, displayed frames, dropped frames,
and preview FPS. The sensor requires the board's external 24 MHz camera
clock and its standard one-lane MIPI CSI connection.

