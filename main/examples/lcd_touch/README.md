# RGB888 LCD and interrupt-driven touch

This example initializes the display through `bsp::Board`, draws red, green,
blue, white and black color bars, then enables the backlight. It also initializes
touch and prints touch coordinates when a touch interrupt wakes the application task.

The same source supports all three hardware profiles:

| Hardware version | Display driver | Resolution | Touch driver | Interrupt GPIO |
| --- | --- | --- | --- | --- |
| `standard` | JD9365 | 720 x 720 | Jadard | 6 |
| `round` | JD9365DA | 720 x 720 | GT911 | 6 |
| `rect` | ILI9882 | 720 x 1440 | ILI9882 | 4 |

`initializeDisplay()` returns `esp_lcd_panel_handle_t` for every profile. The
BSP selects the driver, power sequence, reset pins and DSI timing at build
time. Application code uses the common ESP-IDF panel API without hardware
conditionals:

```cpp
const esp_lcd_panel_handle_t display = initializeDisplay();
ESP_ERROR_CHECK(drawColorBars(display));
ESP_ERROR_CHECK(board.setBacklightBrightness(100));
```

The example uses one RGB888 framebuffer (three bytes per pixel, R/G/B) and
24-bit panel output. Screen dimensions come from `bsp::Board::displayWidth`
and `bsp::Board::displayHeight`. Initialization errors stop the example through
`ESP_ERROR_CHECK`.

The file-scope `board` owns the panel and framebuffer. The returned handle is
borrowed and remains valid until `board.deinitializeDisplay()`,
`board.deinitialize()`, or Board destruction. Use Board APIs for teardown;
do not call `esp_lcd_panel_del()` on the borrowed handle.

`initializeTouch()` returns a borrowed `bsp::TouchController *` for every
profile. It supplies an interrupt callback through `bsp::TouchConfig`, using
the BSP default edge: falling for standard/round and rising for rect.
The ISR only sends a FreeRTOS task notification. The application task blocks on
`ulTaskNotifyTake()` and reads a common `bsp::TouchData` report over I2C after
being notified; it does not periodically poll. A single startup read handles
any report that became ready during initialization.

Each active point is logged with its index, ID, X/Y coordinates and strength.
A successful zero-contact report logs release after an active touch. A `busy`
result means no fresh report and preserves the previous touch state; other
read errors are logged. The BSP selects each controller's default I2C address,
bus speed and reset sequence. Touch resources also belong to `board`.

On round hardware, GT911 initialization first tries `0x5D`, then selects and
tries `0x14` with a second reset if the first address does not ACK. The log
prints the address actually in use. `TouchConfig::try_alternate_i2c_address`
can disable this fallback; bus timeouts return immediately.

On rect hardware, the BSP first probes `0x41`. A rejected ICE chip-ID query
logs a warning and initialization continues with mandatory protocol/firmware
queries. Failure of either required query still stops initialization.

Build from an exported ESP-IDF PowerShell terminal:

```powershell
.\build.ps1 -Example lcd_touch -HardwareVersion standard
.\build.ps1 -Example lcd_touch -HardwareVersion round
.\build.ps1 -Example lcd_touch -HardwareVersion rect
```

Add `-Action flash-monitor -Port COM198` to flash and monitor the selected board.
