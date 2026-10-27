# T-Panel-P4 Rect BSP

This component owns board wiring and hardware initialization order for the
RECT T-Panel-P4. Chip-level register behavior remains in
`peripheral_drivers_lib`.

## Owned resources

- ESP-IDF I2C master bus on GPIO3/GPIO2
- AXP517 power-management/charger at address `0x34`, with GPIO4 interrupt
- GPIO23 LEDC backlight PWM
- MIPI PHY LDO channel 3 at 2.5 V
- MIPI DSI bus, DBI command IO, DPI panel, and framebuffers
- Ilitek ILI9882 touch controller at `0x41` and GPIO4 interrupt
- I2S0 full-duplex audio on GPIO9-GPIO13
- ES8389 codec at 7-bit address `0x10`
- GPIO31 speaker-amplifier enable
- SDMMC slot 0 in 4-bit mode on GPIO43/GPIO44/GPIO39-GPIO42
- SD-card power through the on-chip LDO channel 4
- Direct LCD reset GPIO45 and touch reset GPIO22

The HD0549S001 ILI9882 command table and 720x1440 RGB565 timing are supplied
by `peripheral_drivers_lib`. The generic ILI9882 driver receives the vendor
sequence automatically, or a board may provide its own static sequence through
`Ili9882Config::init_sequence`.

## Basic use

```cpp
#include <bsp/t_panel_p4_rect.hpp>

bsp::t_panel_p4_rect::Board board;

ESP_ERROR_CHECK(board.initialize());
ESP_ERROR_CHECK(board.initializeDisplay());

void *framebuffer = board.frameBuffer();
esp_lcd_panel_handle_t panel = board.displayPanel();

// Fill and submit the framebuffer before enabling the backlight.
ESP_ERROR_CHECK(board.setBacklightBrightness(100));
```

`initialize()` creates shared board resources. `initializeDisplay()` is
separate so applications that do not use the LCD avoid allocating a full
framebuffer. `deinitialize()` releases resources in reverse dependency order.

`initialize()` also initializes the board-owned AXP517 power service. Its
GPIO4 ISR only wakes a FreeRTOS task; that task reads and clears the AXP517
interrupt registers, reads power and fault status, and invokes the registered
`PowerEventCallback` in task context. Applications can use the Board battery,
VBUS, fault, and power-status APIs without creating an AXP517 transport.

## Camera controls

`initializeCamera()` keeps its existing behavior and starts the OV2710 stream.
Applications can pause and resume the stream with `stopCameraStreaming()` and
`startCameraStreaming()`. `reconfigureCamera()` safely rebuilds the CSI/ISP
pipeline and capture buffers for another supported OV2710 mode, retaining the
previous configuration if setup fails. `recoverCameraPipeline()` performs the
same rebuild for CSI/ISP recovery.

The BSP also exposes the OV2710 product-ID, software reset, test-pattern,
mirror, flip, auto-exposure, and sensor-statistics operations without exposing
the board-private transport objects. `cameraStatistics()` includes received
and dropped frames plus pipeline recovery/error counters.

Additional board controls are available for the speaker amplifier, SD power,
and touch-controller reset. Pin constants are declared in
`bsp/t_panel_p4_rect_pins.hpp`.

## Power modes

The BSP exposes ship, light-sleep, and deep-sleep entry points. Ship mode
uses the AXP517 to cut the battery BATFET. The board can remain powered when
external VBUS is present.

```cpp
ESP_ERROR_CHECK(board.enterShipMode());
```

Light sleep returns after wakeup. Its default wake source is the board button
on GPIO6, active low. Stop and deinitialize camera, audio, and SD-card
operations first; the BSP rejects light sleep while those optional resources
are active. An optional timer can be enabled together with the button.

```cpp
bsp::t_panel_p4_rect::LightSleepConfig light_sleep{};
light_sleep.wakeup_gpio = bsp::t_panel_p4_rect::pins::button;
light_sleep.wakeup_on_high = false;
light_sleep.timer_wakeup_us = 30'000'000;
light_sleep.suspend_display = true;
ESP_ERROR_CHECK(board.enterLightSleep(light_sleep));
```

Deep sleep does not return on success and performs a complete BSP shutdown.
ESP32-P4 only supports GPIO0 through GPIO15 as deep-sleep GPIO wake sources,
so the GPIO36 board button cannot wake deep sleep. Configure a valid low-numbered
board signal, a timer, or leave both disabled when reset/power-cycle-only wakeup
is intended.

```cpp
bsp::t_panel_p4_rect::DeepSleepConfig deep_sleep{};
deep_sleep.wakeup_gpio = GPIO_NUM_NC;
deep_sleep.timer_wakeup_us = 60'000'000;
ESP_ERROR_CHECK(board.enterDeepSleep(deep_sleep));
```

Before deep sleep, the BSP shuts down all owned peripherals and holds the LCD
backlight and I2S output pins low. `Board::initialize()` releases those GPIO
holds after a deep-sleep reset.

## SDK configuration profiles

The root project automatically prepends `sdkconfig.defaults` from this BSP for
every selected example. It contains the audio-player contract used by
`music.cpp`: file playback, 48 kHz resampling, stereo channel conversion, and
16-bit output conversion. This profile is loaded globally because the BSP
music implementation is part of the component even when an application does
not use the music API. An application's `sdkconfig.defaults` may override
these values when it intentionally chooses a different audio contract.

`sdkconfig.lvgl.defaults` is kept separate and is loaded only when the selected
example declares an `lvgl/lvgl` dependency. LVGL allocates substantial PSRAM
and draw-buffer resources, so it should not be enabled for headless audio,
camera, storage, or connectivity applications merely because they use the
same board BSP.

## LVGL performance profile

`sdkconfig.lvgl.defaults` contains the board-recommended LVGL configuration:
RGB565, the FreeRTOS backend, two software draw units, ESP32-P4 PPA drawing,
64-byte draw-buffer alignment, style caching, a 16 ms refresh period, and a
1000 Hz FreeRTOS tick. The root project automatically prepends this profile
when the selected example declares an `lvgl/lvgl` component dependency.

Keep application-specific options such as enabled widgets, fonts, demos, and
performance monitors in the application's own `sdkconfig.defaults`. For a
separate project, add the BSP profile followed by the application defaults to
the `SDKCONFIG_DEFAULTS` CMake list. Existing generated `sdkconfig` files keep
their explicit values; use a new build directory or run `fullclean` when first
adopting the profile.

## SD card

SD-card mounting is optional, so `initialize()` does not mount it. Call
`initializeSdCard()` after the common board initialization. The default
configuration mounts the primary SD slot at `/sdcard` in SDMMC 4-bit mode at
20 MHz. Automatic formatting is disabled by default.

```cpp
#include <cstdio>

#include <bsp/t_panel_p4_rect.hpp>

bsp::t_panel_p4_rect::Board board;
ESP_ERROR_CHECK(board.initialize());

bsp::t_panel_p4_rect::SdCardConfig config{};
config.mount_point = "/sdcard";
config.format_if_mount_failed = false;
ESP_ERROR_CHECK(board.initializeSdCard(config));

FILE *file = std::fopen("/sdcard/test.txt", "w");
if (file == nullptr) {
    // Handle the filesystem error here.
} else {
    std::fputs("T-Panel-P4\n", file);
    std::fclose(file);
}

ESP_ERROR_CHECK(board.deinitializeSdCard());
ESP_ERROR_CHECK(board.deinitialize());
```

`sdCard()` exposes the active `sdmmc_card_t *` for SDMMC card information and
lower-level operations. `sdCardMountPoint()` exposes the active VFS path.
Both return `nullptr` when the card is not mounted. Close every open file
before calling `deinitializeSdCard()`; the BSP then unmounts FatFS before it
turns off the on-chip SD LDO.

Block-device owners such as USB MSC must initialize the card without mounting
FatFS locally:

```cpp
bsp::t_panel_p4_rect::SdCardConfig config{};
config.mount_filesystem = false;
ESP_ERROR_CHECK(board.initializeSdCard(config));

sdmmc_card_t *raw_card = board.sdCard();
```

In this mode `sdCard()` returns the initialized raw card, while
`isSdCardMounted()` is false and `sdCardMountPoint()` returns `nullptr`. Only
one block-device owner may access the card at a time. Release that owner before
calling `deinitializeSdCard()`.

When `CONFIG_TINYUSB_MSC_ENABLED` is enabled, the BSP can initialize the raw SD
card and expose it directly through the ESP32-P4 USB OTG port:

```cpp
ESP_ERROR_CHECK(board.initialize());
ESP_ERROR_CHECK(board.initializeUsbMsc());
```

`initializeUsbMsc()` automatically initializes an unmounted raw SD card when
needed. `deinitializeUsbMsc()` stops TinyUSB, removes the MSC storage, and
releases that SD card. Calling `deinitializeSdCard()` while USB MSC is active
returns `ESP_ERR_INVALID_STATE` to prevent disconnecting live block storage.
When TinyUSB MSC is disabled in sdkconfig, `initializeUsbMsc()` returns
`ESP_ERR_NOT_SUPPORTED` and the MSC implementation is compiled out.

## Audio

Audio initialization is optional. `initializeAudio()` creates an I2S0
full-duplex channel pair, configures the ES8389 over the shared I2C bus,
starts both I2S channels, enables the codec, and optionally enables the
speaker amplifier through GPIO31. The default format is 48 kHz, 16-bit,
stereo I2S with a 256x MCLK.

```cpp
bsp::t_panel_p4_rect::Board board;
ESP_ERROR_CHECK(board.initialize());

bsp::t_panel_p4_rect::AudioConfig config{};
config.sample_rate_hz = 48000;
config.word_length = pdl::audio::Es8389WordLength::bits16;
config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
config.output_volume_db = -12.0F;
config.microphone_gain = pdl::audio::Es8389MicrophoneGain::db12_5;
config.enable_speaker = true;
ESP_ERROR_CHECK(board.initializeAudio(config));

std::int16_t samples[512]{};
std::size_t bytes_read{};
std::size_t bytes_written{};
ESP_ERROR_CHECK(board.readAudio(samples, sizeof(samples), bytes_read));
ESP_ERROR_CHECK(board.writeAudio(samples, bytes_read, bytes_written));

ESP_ERROR_CHECK(board.deinitializeAudio());
```

`audioCodec()` exposes the initialized PDL ES8389 driver for runtime volume,
mute, and microphone-gain changes. `audioTransmitChannel()` and
`audioReceiveChannel()` expose the ESP-IDF handles for applications that need
nonblocking I2S callbacks or direct channel operations. ES8389 uses 7-bit I2C
address `0x10`; the `0x20` value in the original Espressif codec header is an
8-bit wire address and must not be passed directly to `I2cDevice`.

The BSP also provides `reconfigureAudio()` for a controlled runtime format
change. It stops and destroys the active file player before rebuilding the I2S
and codec path, so callers can switch sample rate, word length, slot mode, DMA
settings, or capture mode without racing the playback callback. Runtime
controls include `setAudioVolumeDb()`, `setAudioMuted()`,
`setAudioOutputEnabled()`, `setAudioCaptureEnabled()`,
`setAudioCaptureMuted()`, and `setMicrophoneGain()`. The capture mute is a
software mute applied after I2S reads; the microphone gain is programmed in the
ES8389.

`audioStatistics()` reports TX/RX calls, transferred bytes, timeouts, errors,
and short transfers. Call `resetAudioStatistics()` before measuring a specific
workload. `deinitializeAudio()` first waits for the BSP music task and its
output callback to stop, then releases the codec and I2S resources.

The BSP also provides a WAV recorder. It owns a capture task, a writer task,
and a FreeRTOS stream buffer between them. The writer patches the RIFF/WAVE
header with the final data length when recording stops. The data callback is
invoked after each chunk is written; its `data` pointer is valid only while the
callback is running.

```cpp
void onRecordingEvent(
    const bsp::t_panel_p4_rect::RecordingEvent &event,
    void *)
{
    if (event.type == bsp::t_panel_p4_rect::RecordingEventType::data) {
        // Consume or copy event.data/event.data_size here.
    }
}

ESP_ERROR_CHECK(board.setRecordingEventCallback(onRecordingEvent));
bsp::t_panel_p4_rect::RecordingConfig recording{};
recording.path = "/sdcard/recordings/mic.wav";
recording.ring_buffer_bytes = 64U * 1024U;
recording.chunk_bytes = 4096U;
ESP_ERROR_CHECK(board.startRecording(recording));
// ... application work ...
ESP_ERROR_CHECK(board.stopRecording());
```

`recordingState()`, `recordingLastError()`, `recordingBytes()`,
`recordingDurationMs()`, and `isRecording()` expose the state-machine status.
Stopping or deinitializing audio drains the ring buffer and closes the WAV file
before the I2S RX channel is released.

Music playback is serialized by a dedicated BSP task. `playMusicFile()`,
`pauseMusic()`, and `stopMusic()` wait for the command to be accepted by the
GMF player and return the actual operation result. Register a
`MusicEventCallback` with `setMusicEventCallback()` to receive state changes
and decoded format information. `probeMusicFile()` parses MP3, WAV, FLAC, and
ADTS AAC headers before playback; the overload of `playMusicFile()` accepting
`MusicPlaybackConfig` can automatically reconfigure the BSP output sample rate
before the decoder starts. `musicFormat()`, `musicDurationMs()`,
`musicLastError()`, and `musicStatistics()` expose the current diagnostics.
Keep callbacks short and use them to notify application code; do not call
player control methods recursively from inside a callback.
`seekMusicMs()` returns `ESP_ERR_NOT_SUPPORTED` because the ESP Audio Simple
Player API used by this BSP does not expose a seek primitive; it no longer
pretends to seek by changing only the UI position. Playlist and repeat policy
remain application/service concerns rather than board-driver state.
