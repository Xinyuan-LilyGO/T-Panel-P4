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
- LCD VCI power-enable GPIO45 and touch reset GPIO22
- QMI8658C six-axis IMU at `0x6A` or `0x6B` and QMC6309 magnetometer at `0x7C`
- DRV2605 haptic driver at `0x5A`, enabled by GPIO21; initialized in standby

The HD0549S001 ILI9882 command table is supplied by `peripheral_drivers_lib`.
The generic ILI9882 driver receives the vendor sequence automatically, or a
board may provide its own static sequence through `Ili9882Config::init_sequence`.
The 720x1440 scan timing is selected in the shared `src/board.cpp`. Display
initialization holds VCI low while preparing DSI, then reproduces the old
driver's GPIO45 waveform: high 5 ms, low 10 ms, high 120 ms before sending the
vendor command table. GPIO45 switches only the 2.8 V VCI rail; IOVCC remains
powered from 3.3 V and the LCD RESET pin has an RC pull-up. GPIO45 is therefore
a power enable, rather than a direct LCD reset signal.

Display defaults are RGB888 input, two framebuffers and a 24-bit panel; see the shared
BSP README for `bsp::DisplayConfig` overrides. DCS diagnostics are disabled by
default. When `read_display_id` is enabled, the BSP reads ID (`0x04`)
before initialization with bounded FIFO waits, logs DSI status,
and stops initialization on a timeout. This avoids the ESP-IDF 5.5 receive function's
unbounded RX FIFO wait. Command ACK is enabled as in the old driver. DBI writes
use the same packet formats with bounded FIFO waits. BTA is enabled by the ID
read, or directly when diagnostics are disabled. After starting DPI, the BSP
sends DISPON alone as in the old driver. The ID is not
needed to send the initialization sequence or start display output.

Touch initialization probes `0x41` after hardware reset, then checks the
Ilitek protocol and firmware version. ICE chip-ID reads are best effort:
production firmware may reject debug-mode commands while normal touch
commands still work. Protocol/firmware failures remain fatal. Panel and
report-format queries use the normal command without an F6 preamble, with
legacy core-version support. The default GPIO4 interrupt uses a rising edge,
matching the rect reference driver; the application reads coordinates in the
notified task, outside the ISR.

## Basic use

```cpp
#include <bsp/api/board.hpp>

bsp::t_panel_p4_rect::Board board;

ESP_ERROR_CHECK(board.initializeDisplay());

void *framebuffer = board.frameBuffer();
esp_lcd_panel_handle_t panel = board.displayPanel();

// Fill and submit the framebuffer before enabling the backlight.
ESP_ERROR_CHECK(board.setBacklightBrightness(100));
```

`initialize()` initializes the entire board: power, display, touch, audio, camera, sensors, motor, and SD card.
It uses default configurations only for devices that are not already ready.
An absent device (including a missing SD card) returns an error. Applications
that need fewer devices, custom settings, or optional-device error handling
should call the corresponding `initializeXxx()` methods directly, as above.

Pass `false` to `initialize(false)` to skip SD-card initialization while keeping
the other default peripherals and their rollback behavior. A later call to the
no-argument `initialize()` can initialize the SD card when it becomes available.

```cpp
#include <bsp/api/board.hpp>

bsp::Board board;
ESP_ERROR_CHECK(board.initialize()); // All default peripherals; requires an SD card.
// Camera is ready, but capture is explicitly started by the application.
ESP_ERROR_CHECK(board.startCameraStreaming());
```

Every device initializer automatically prepares its dependencies. For example,
`initializeAudio()` prepares power and I2C before configuring I2S and ES8389;
`initializeDisplay()` prepares power, backlight PWM (initially off), and LCD.
No earlier call to `initialize()` is required. Repeated initialization reuses
the existing object and preserves its configuration; use `reconfigureAudio()`
or `reconfigureCamera()` for those devices, or explicitly tear down a device
before initializing it with different settings.

`initializeI2cBus()` creates only the shared bus with board pins and internal
pull-ups. `isI2cBusInitialized()` reports its readiness. `initializePower()` prepares these dependencies and the
AXP517 service; repeating it preserves the existing `PowerConfig`.

`isInitialized()` is true only when all default peripherals are ready. Use
`isAudioInitialized()`, `isPowerInitialized()`, etc. to check a single device.
`devices()` exposes each ready device even when `isInitialized()` is false.
If full initialization fails, it releases only peripherals created by that
call, in reverse order. Previously ready devices retain their objects and
configuration. Shared buses, IO expander, and board power remain available
for retry until `deinitialize()`. Individual teardown releases that device's
resources; `deinitializeDisplay()` also releases its backlight PWM.

Initialization prepares hardware without starting camera capture, music,
recording, LoRa transmission, or USB MSC. These actions use explicit APIs.
Serialize initialization, teardown, and direct driver operations externally.

`initialize()` also initializes the board-owned AXP517 power service. Its
GPIO4 ISR only wakes a FreeRTOS task; that task reads and clears the AXP517
interrupt registers, reads power and fault status, and invokes the registered
`PowerEventCallback` in task context. Applications can use the Board battery,
VBUS, fault, and power-status APIs without creating an AXP517 transport.

## Device access

The build-selected `<bsp/api/board.hpp>` header provides `bsp::Board` and
`bsp::Devices` for applications shared by standard, round, and rect hardware.
The board-specific header and namespace remain available.

```cpp
#include <bsp/api/board.hpp>

bsp::Board board;
ESP_ERROR_CHECK(board.initializePower());
auto bsp_device = board.devices();
auto *axp517 = bsp_device.axp517;
if (axp517 != nullptr)
{
    std::uint8_t percent{};
    const pdl::Status status = axp517->readBatteryPercentage(percent);
    if (pdl::isError(status))
        std::printf("Battery read failed: %s\n", pdl::statusName(status));
}
```

`devices()` returns a non-owning snapshot containing `i2c_bus`, `axp517`,
`touch`, `es8389`, `ov2710`, `sgm38121`, `display_panel`, `dsi_bus`,
`display_io`, `i2s_tx`, `i2s_rx`, and `sd_card`. Optional devices become
available after their corresponding `initialize*()` call. Uninitialized
resources are `nullptr`; rect hardware has no XL9555 or LoRa members.
`es8389` is an `Es8389Device` adapter whose operations return `esp_err_t`.
`audio_codec` and `audio_input_codec` expose the ES8389 output and ES7210
input `esp_codec_dev_handle_t` handles. The BSP owns both audio chips;
applications must not initialize another ES7210 driver on the same device.

The Board owns all returned resources. Reacquire the snapshot after
initialization, teardown, reconfiguration, or sleep/wake, and never use saved
pointers after the resource or Board is destroyed. Use Board APIs for
lifecycle, camera streaming, power/reset, and audio controls that maintain BSP
settings; direct chip writes bypass those settings. Serialize direct device
access against BSP operations and background tasks.

## Camera controls

`initializeCamera()` prepares OV2710 and the CSI/ISP pipeline without starting
capture. Call `startCameraStreaming()` explicitly. Applications can pause and
resume capture with `stopCameraStreaming()` and `startCameraStreaming()`. `reconfigureCamera()` safely rebuilds the CSI/ISP
pipeline and capture buffers for another supported OV2710 mode, retaining the
previous configuration if setup fails. `recoverCameraPipeline()` performs the
same rebuild for CSI/ISP recovery.

The BSP also exposes the OV2710 product-ID, software reset, test-pattern,
mirror, flip, auto-exposure, and sensor-statistics operations without exposing
the board-private transport objects. `cameraStatistics()` includes received
and dropped frames plus pipeline recovery/error counters.

Additional board controls are available for the speaker amplifier, SD power,
and touch-controller reset. Pin constants are declared in
`bsp/pins/pins.hpp`.

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

Before deep sleep, the BSP shuts down all owned peripherals and holds the
backlight, I2C, I2S outputs, SD clock, speaker/motor enables, touch reset, LCD
power and camera flash enables low. The SD command/data bus remains isolated
because its power rail stays on. The selected wakeup GPIO, console, external
outputs/IRQs, buttons, USB and memory pads are not forced low. The console uses
sleep isolation. I2C external pull-ups still draw current when held low.
`initializeI2cBus()` releases the holds after a deep-sleep reset; backlight
initialization releases its own hold after preloading a safe low level.

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

`initialize()` includes SD-card initialization and mounting. To initialize only
the SD card, call `initializeSdCard()` directly. The default configuration mounts the primary SD slot at `/sdcard` in SDMMC 4-bit mode at
20 MHz. Automatic formatting is disabled by default.

```cpp
#include <cstdio>

#include <bsp/api/board.hpp>

bsp::t_panel_p4_rect::Board board;

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
ESP_ERROR_CHECK(board.initializeUsbMsc());
```

`initializeUsbMsc()` automatically initializes an unmounted raw SD card when
needed. `deinitializeUsbMsc()` stops TinyUSB, removes the MSC storage, and
releases that SD card. Calling `deinitializeSdCard()` while USB MSC is active
returns `ESP_ERR_INVALID_STATE` to prevent disconnecting live block storage.
When TinyUSB MSC is disabled in sdkconfig, `initializeUsbMsc()` returns
`ESP_ERR_NOT_SUPPORTED` and the MSC implementation is compiled out.

## Audio

For audio alone, call `initializeAudio()` directly. It creates an I2S0
full-duplex channel pair and configures ES8389 output plus ES7210 MIC1/MIC2
input through `esp_codec_dev`. ES8389 ADC/PGA are disabled and its ASDOUT is
high impedance. Initialization uses I2C retries, early I2S clocks, chip-ID
validation and DVDD reset readback, and leaves the GPIO31 amplifier off. The default format is 48 kHz, 16-bit,
stereo I2S with a 384x MCLK.

```cpp
bsp::t_panel_p4_rect::Board board;

bsp::t_panel_p4_rect::AudioConfig config{};
config.sample_rate_hz = 48000;
config.word_length = bsp::t_panel_p4_rect::AudioWordLength::bits16;
config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
config.output_volume_db = -12.0F;
config.microphone_gain = bsp::t_panel_p4_rect::AudioMicrophoneGain::db12_5;
config.enable_speaker = true;
ESP_ERROR_CHECK(board.initializeAudio(config));

std::int16_t samples[512]{};
std::size_t bytes_read{};
std::size_t bytes_written{};
ESP_ERROR_CHECK(board.readAudio(samples, sizeof(samples), bytes_read));
ESP_ERROR_CHECK(board.setAudioOutputEnabled(true));
ESP_ERROR_CHECK(board.writeAudio(samples, bytes_read, bytes_written));
const esp_err_t drain_result = board.drainAudio();
ESP_ERROR_CHECK(board.setSpeakerEnabled(false));
ESP_ERROR_CHECK(drain_result);

ESP_ERROR_CHECK(board.deinitializeAudio());
```

All profiles use the shared audio lifecycle documented in the
[BSP README](../../README.md). `enable_speaker` permits playback; call
`setAudioOutputEnabled(true)` before writing PCM. Stop all TX writers and
call `drainAudio()` before shutting the PA down on normal completion;
disable PA first for cancellation or failure.

`audioCodec()` exposes the ES8389 output `esp_codec_dev_handle_t`.
`audioInputCodec()` exposes the separate ES7210 input handle. Use Board APIs
for volume/mute state and ES7210 microphone-gain changes. `audioTransmitChannel()` and
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
`setAudioCaptureMuted()`, and `setMicrophoneGain()`. Capture mute is programmed and verified in the
ES7210 ADC; successful muted reads are also zero-filled in software.
Microphone gain is programmed in the ES7210 (quantized to its supported gain steps).

### MIC3 AEC reference

The board's AEC circuit feeds the ES8389 analog playback output into ES7210
MIC3. Enable this reference explicitly; the default remains MIC1/MIC2 capture.
This supplies synchronized microphone/reference PCM for an application AEC
algorithm; the BSP does not perform echo cancellation.

```cpp
namespace rect = bsp::t_panel_p4_rect;
rect::Board board;
rect::AudioConfig config{};
config.enable_aec_reference = true;
config.aec_reference_gain = rect::AudioMicrophoneGain::db0;
ESP_ERROR_CHECK(board.initializeAudio(config));

// 256 capture frames: MIC1, MIC2, MIC3 reference, repeated in that order.
std::int16_t samples[256 * 3]{};
std::size_t bytes_read{};
ESP_ERROR_CHECK(board.readAudio(samples, sizeof(samples), bytes_read));
const std::size_t frames_read = bytes_read / (3 * sizeof(std::int16_t));
// Frame i: samples[3*i], samples[3*i+1], samples[3*i+2].
// Feed these microphone/reference samples to the application's AEC algorithm.
(void)frames_read;

ESP_ERROR_CHECK(board.setMicrophoneGain(
    rect::AudioMicrophoneGain::db0, rect::AudioMicrophoneChannel::reference));
ESP_ERROR_CHECK(board.deinitializeAudio());
```

This mode requires `enable_capture=true`, 16-bit samples, stereo slot mode and
`I2S_STD_SLOT_BOTH`. Unsupported formats and boards return
`ESP_ERR_NOT_SUPPORTED`. RX uses four 16-bit TDM wire slots, selecting slots
0/1/2 for MIC1/MIC2/MIC3 and discarding the unused MIC4 slot. TX continues to
accept stereo 16-bit PCM in two 32-bit wire slots; both paths use 64 BCLKs per
sample frame. The BSP verifies the applied slot and clock configuration during
initialization. `audioReceiveChannel()` exposes this TDM RX handle.

`audioCaptureChannelCount()` reports 3 while capture is enabled. Every
`readAudio()` request must contain whole six-byte capture frames; other sizes
return `ESP_ERR_INVALID_SIZE`. Do not write the three-channel buffer directly
to the stereo output: extract or process the microphone channels first.
`aec_reference_gain` defaults to 0 dB independently of `microphone_gain`.
`AudioMicrophoneChannel::both` still adjusts MIC1/MIC2 only. Capture mute covers
all three channels. `no_dac_reference` configures the ES8389's own ADC reference
path and does not enable or disable MIC3.

The WAV recorder saves all three captured channels with a matching WAV header.
Its chunk size is rounded down to a whole six-byte frame (4096 becomes 4092),
so `startRecording(path)` also works with its default configuration. The
resulting three-channel WAV is intended for capture analysis or AEC processing.
Stop application RX/TX users before calling `reconfigureAudio()` to change
`enable_aec_reference`; the existing BSP lifecycle also stops its own recording
and music workers.

`audioStatistics()` reports TX/RX calls, transferred bytes, timeouts, errors,
and short transfers. Call `resetAudioStatistics()` before measuring a specific
workload. `deinitializeAudio()` first stops BSP recording and music workers, then
releases both codec paths and I2S resources.

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
