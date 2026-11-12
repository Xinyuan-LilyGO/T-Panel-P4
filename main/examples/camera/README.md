# OV2710 camera preview

The shared T-Panel-P4 BSP owns the complete camera lifecycle: SGM38121
power sequencing, OV2710 configuration, MIPI CSI and ISP setup, RGB PSRAM
capture buffers, frame synchronization, streaming, and cleanup. Its default
camera configuration captures 1920x1080 RAW10 and converts it to RGB565.

This example initializes the display, camera and PPA before starting camera
streaming. It uses PPA to center-crop the camera frame to the selected display's
aspect ratio: 960x960 scaled to 720x720 on standard/round, or 480x960 scaled to
720x1440 on rect. The example selects RGB888 capture and display input,
independently of the 24-bit panel output. The three RGB888 capture buffers use
about 17.8 MiB of PSRAM. All board profiles share the same camera implementation and
DMA buffer protection. Pass a `CameraConfig` to `initializeCamera()` to select
the sensor mode, input/output color formats, two or three capture buffers,
exposure target, ISP clock, and PSRAM fallback policy.

## Color formats

`CameraConfig::input_data_color_type` describes the sensor/CSI input and must
match the sensor mode. The current OV2710 modes require `CAM_CTLR_COLOR_RAW10`;
other input formats are rejected with `ESP_ERR_INVALID_ARG`. The sensor format
is not changed by assigning this field.

`CameraConfig::output_data_color_type` selects `CAM_CTLR_COLOR_RGB565` (default)
or `CAM_CTLR_COLOR_RGB888`. The BSP maps it to the corresponding CSI and ISP
formats and allocates two or three bytes per pixel respectively. Unsupported
output formats are rejected before camera resources are allocated.

Set `kCameraOutputColorType` in `main.cpp` to `CAM_CTLR_COLOR_RGB888` (the
example's current selection) or `CAM_CTLR_COLOR_RGB565`. This single constant also selects the PPA input and
output formats, LCD framebuffer input format, and frame size checks. RGB888
uses 50% more pixel storage: three 1920x1080 capture buffers occupy about
17.8 MiB, plus about 4.45 MiB for three square display buffers or 8.90 MiB for
three portrait display buffers. Measure throughput and dropped frames on the
board after changing formats. The synchronization below applies to both formats.

On Round, the existing panel BGR MADCTL setting was calibrated using the
`lcd_touch` color bars, which write R/G/B bytes. ESP-IDF's native RGB888 pixel
layout uses B/G/R bytes; passing the camera/PPA RGB888 output straight to that
framebuffer swaps red and blue. The preview therefore enables PPA `rgb_swap`
only for Round RGB888, as part of the existing scale operation. RGB565 and the
other board profiles retain their existing processing. The startup pipeline log
reports whether the R/B exchange is enabled. This correction belongs at the
camera/display boundary; changing the shared panel MADCTL would also change
the color-bar and UI display behavior.

Build it in a dedicated directory:

```powershell
.\build.ps1 -HardwareVersion standard -Example camera -Action build
.\build.ps1 -HardwareVersion rect -Example camera -Action build
```

Use `round` for the round panel. Each combination has an independent directory
under `build/<hardware>-camera`.

## Preview pipeline and synchronization

PPA writes directly into one of three BSP-owned display framebuffers.
Submitting that buffer to the DPI panel selects it without an extra application
frame copy. Crop geometry, PPA parameters and borrowed display handles are
prepared once; each frame only updates the input/output buffer addresses.

The example uses `on_refresh_done` to synchronize framebuffer rotation.
`on_color_trans_done` is unsuitable here: ESP-IDF calls it synchronously inside
`draw_bitmap()` when the supplied buffer is already a DPI framebuffer, before
the screen has finished refreshing. After each submission the example clears
any old refresh notification and waits for a future refresh before processing
the next frame. Three display buffers retain an extra rotation of separation
between PPA writes and scanout.

The refresh wait happens before acquiring the next camera frame, so a stalled
display cannot make the application retain an old capture buffer or repeatedly
run PPA on frames it cannot submit. Blocking PPA finishes before the camera
frame is released, and release happens before any display submission. PPA and
display failures end the preview and run cleanup; display refresh has a 100 ms
timeout. Ordinary camera frame timeouts are logged and retried. A reclaimed
camera frame notification yields for one scheduler tick before retrying.

Cleanup releases camera resources, unregisters PPA, and stops the display before
deleting the semaphore used by its ISR. The application does not reconfigure
the BSP while running; any extension that does so must reacquire cached panel
and framebuffer pointers.

## Performance measurements

Approximately once per second, the serial log reports submission FPS, average
blocking PPA time, average display wait time per submitted frame, and cumulative
received/submitted/dropped/short-frame counts. `submitted` counts successful
`draw_bitmap()` calls, not a hardware count of frames visible on the panel.

The selected sensor mode is 1920x1080 at 25 FPS, so the nominal input rate caps
preview throughput at 25 FPS. PPA time identifies scaling cost; LCD wait time
identifies display pacing. Changing the mode or reducing capture buffers should
be evaluated with these timings and the dropped-frame count on the actual board.
Two capture buffers save one frame of memory but leave less room while a frame
is held; the example keeps three. Approximate memory use is:

| Pixel format | Three 1920x1080 capture buffers | Three square display buffers | Three portrait display buffers |
| --- | --- | --- | --- |
| RGB565 | 11.9 MiB | 3.0 MiB | 5.9 MiB |
| RGB888 | 17.8 MiB | 4.45 MiB | 8.90 MiB |

The sensor requires the board's external 24 MHz camera clock and its standard
one-lane MIPI CSI connection. FPS, tearing and memory-bandwidth behavior require
validation on the selected hardware.

