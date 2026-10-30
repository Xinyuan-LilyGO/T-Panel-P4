# LVGL FPS benchmark

This example runs LVGL 9.5.0's official `lv_demo_benchmark()` on the selected
T-Panel-P4 hardware version. The board selection is compile-time and supports
the 720 x 720 standard/round panels and the 720 x 1440 rect panel, including
their respective display and touch controllers.

The benchmark cycles through the official scenes and shows the current FPS,
render time, CPU usage, and final benchmark summary on the display. The same
per-scene results and all-scenes average are written to the serial monitor.
The Jadard touch controller is registered as an LVGL pointer input device, so
interactive benchmark widgets can be operated from the panel. Avoid touching
the display while collecting comparable benchmark results because input
handling and UI interaction can change the measured workload.

LVGL renders directly into the two framebuffers allocated by the ESP-IDF
MIPI-DPI driver. Buffer swaps are synchronized to the display refresh event,
so the flush path performs cache write-back and framebuffer switching without
copying each 48-line block into a separate display buffer.

Configure and build from an ESP-IDF 5.5.3 terminal:

```powershell
idf.py -B build/lvgl_fps_test `
    -D T_PANEL_P4_HARDWARE_VERSION=standard `
    -D T_PANEL_P4_EXAMPLE=lvgl_fps_test build
```

Use `standard`, `round`, or `rect` for `T_PANEL_P4_HARDWARE_VERSION` and keep
the build directory separate for each hardware version.

Flash and open the serial monitor:

```powershell
idf.py -B build/lvgl_fps_test flash monitor
```

Use the final `All scenes avg.` FPS shown by the official benchmark when
comparing changes. This example uses a 16 ms LVGL refresh period and
performance compiler optimization. FreeRTOS uses a 1000 Hz tick so the LVGL
task can service the 16 ms refresh timer without the 10 ms quantization of the
default 100 Hz tick. The 64 KiB simple-layer buffer reduces the number of
rendering chunks used by translucent layers. Keep these settings unchanged
between comparison runs.

The ESP32-P4 PPA backend accelerates supported opaque fills and RGB images.
LVGL uses its FreeRTOS OS backend with two software draw units so both CPU
cores can render larger invalidated areas in parallel. Compile-time assertions
prevent the benchmark from building if these acceleration settings are not
active. LVGL draw buffers use the required 64-byte alignment matching the P4
L2 cache-line size.

The object style cache is enabled to reduce repeated style-property lookup in
widget-heavy scenes. The benchmark also uses LVGL's aligned benchmark fonts;
this improves the bundled benchmark text only. Application fonts need to be
generated with suitable bitmap depth, stride, and alignment to receive the
same benefit.

The scrolling scene is primarily a software-rendering workload. Its ARGB8888
avatars, text, rounded widgets, and alpha blending are not all accelerated by
the PPA backend. For an application UI, prefer opaque RGB565 images and opaque
row backgrounds, keep fixed headers outside the scrolling container, avoid
shadows and layered opacity on moving rows, and instantiate or reuse only the
visible rows.

