# T-Panel-P4 Round BSP

This component owns board wiring and hardware initialization order for the
round T-Panel-P4. Chip-level register behavior remains in
`peripheral_drivers_lib`.

## Owned resources

- ESP-IDF I2C master bus on GPIO3/GPIO2
- XL9555 at address `0x20`
- AXP517 power-management/charger at address `0x34`, with GPIO4 interrupt
- GPIO34 LEDC backlight PWM
- MIPI PHY LDO channel 3 at 2.5 V
- MIPI DSI bus, DBI command IO, DPI panel, and framebuffers
- GT911 touch controller at `0x5D` (alternate `0x14`) and GPIO6 interrupt
- I2S0 full-duplex audio on GPIO9-GPIO13
- ES8389 codec at 7-bit address `0x10`
- XL9555 P0_0 speaker-amplifier enable
- PDL-owned SPI2 bus on GPIO28/GPIO27/GPIO26
- LR2021 RadioLib device with GPIO5 interrupt and DIO10 IRQ routing
- SDMMC slot 0 in 4-bit mode on GPIO43/GPIO44/GPIO39-GPIO42
- SD-card power through XL9555 P0_1 and on-chip LDO channel 4
- XL9555-controlled LCD/touch reset and peripheral power outputs

The BOE AV040YNU-N80-1QP01QP1 / JD9365DA-H3 uses the independent PDL
`jd9365da` driver and its opt-in BOE table. The BSP selects 720x720 RGB888,
two DSI lanes at 500 Mbps, a 36 MHz pixel clock, horizontal timing 2/22/68
and vertical timing 5/16/50 (pulse/back porch/front porch).
The BOE module uses BGR panel color order (MADCTL), so RGB888 framebuffer
data displays red and blue correctly.

`initializeTouch()` selects GT911 address 0x5D by driving INT low while
releasing XL9555 P0_4 reset. `TouchConfig::i2c_address = 0x14` selects the
alternate address with INT high. RST stays low for at least 10 ms before
address selection. After reset release, the BSP waits 10 ms, releases INT
directly to an input, and waits 50 ms before probing the configured address.
If the address NACKs, the BSP resets once more to select and probe the other
address. Bus timeouts and other errors return immediately. Set
`TouchConfig::try_alternate_i2c_address = false` to require the configured
address. `touchConfig().i2c_address` reports the address actually in use.
The default I2C speed is 100 kHz; `TouchConfig::i2c_frequency_hz` is configurable.
If hardware reset is disabled, detection only probes the addresses without
driving RST/INT; the caller must have waited for controller startup.
All boards return `TouchController`
and use `TouchData`; no chip-specific data type is needed for reading touch.
`readTouchData()` returns `busy` for no fresh frame, and `ok` with zero contacts
for a release. GT911 product ID and configuration version remain available
through `touchController()->native()`. GT911 software reset, key input and
sleep/wake are not implemented. See the shared BSP README for the common API.

Build the round display/touch example with:

```powershell
.\build.ps1 -HardwareVersion round -Example lcd_touch
```

## Basic use

```cpp
#include <bsp/board.hpp>

bsp::t_panel_p4_round::Board board;

ESP_ERROR_CHECK(board.initializeDisplay());

void *framebuffer = board.frameBuffer();
esp_lcd_panel_handle_t panel = board.displayPanel();

// Fill and submit the framebuffer before enabling the backlight.
ESP_ERROR_CHECK(board.setBacklightBrightness(100));
```

`initialize()` initializes the entire board: power, display, touch, audio, camera, LoRa, and SD card.
It uses default configurations only for devices that are not already ready.
An absent device (including a missing SD card) returns an error. Applications
that need fewer devices, custom settings, or optional-device error handling
should call the corresponding `initializeXxx()` methods directly, as above.

```cpp
#include <bsp/board.hpp>

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
pull-ups. `isI2cBusInitialized()` reports its readiness. `initializeIoExpander()`
automatically prepares I2C and creates XL9555; `isIoExpanderInitialized()`
reports its readiness. `initializePower()` prepares these dependencies and the
AXP517 service; repeating it preserves the existing `PowerConfig`.
The round board retains its startup GPIO preparation, XL9555 retries, rail
power-off interval, AXP517 configuration, rail settling, and C5 EN release order.
Later device initialization does not repeat the power cycle or reset I2S pins.

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

Applications shared by standard, round, and rect hardware can include the
build-selected `<bsp/board.hpp>` and use `bsp::Board` and `bsp::Devices`.
Each BSP provides the same entry header through its own public include path.

`Board::devices()` returns a `Devices` snapshot of the initialized devices and
native handles owned by that Board. The collection belongs to the BSP because
device selection and lifetime depend on board wiring. Chip drivers remain in
`peripheral_drivers_lib`.

```cpp
#include <bsp/board.hpp>

using namespace bsp::t_panel_p4_round;

Board board;

extern "C" void app_main()
{
    ESP_ERROR_CHECK(board.initializeAudio());

    auto bsp_device = board.devices();
    auto *es8389 = bsp_device.es8389;
    if (es8389 != nullptr)
    {
        std::uint16_t chip_id{};
        ESP_ERROR_CHECK(es8389->readChipId(chip_id)); // Expected: 0x8389
        ESP_ERROR_CHECK(es8389->setVolumeDb(-12.0F));
        ESP_ERROR_CHECK(es8389->setMuted(false));
    }

    // PDL drivers retain their own pdl::Status return type.
    if (auto *axp517 = bsp_device.axp517)
    {
        std::uint8_t percent{};
        const pdl::Status status = axp517->readBatteryPercentage(percent);
        if (pdl::isError(status))
            printf("Battery read failed: %s\n", pdl::statusName(status));
    }
}
```

Use `->` for a device pointer, or `auto &es8389 = *bsp_device.es8389` followed
by `es8389.readChipId(...)` after checking that the pointer is non-null.

| Member | Object / handle | Available after |
| --- | --- | --- |
| `i2c_bus` | PDL ESP-IDF I2C bus | `initializeI2cBus()` or any dependent device |
| `spi_bus` | PDL ESP-IDF SPI bus | `initializeLora()` |
| `xl9555` | `pdl::expand_io::Xl9555 *` | `initializeIoExpander()` or any dependent device |
| `axp517` | `pdl::charge::Axp517 *` | `initialize()` / `initializePower()` |
| `touch` | `TouchController *` | `initializeTouch()` |
| `es8389` | `Es8389Device *` | `initializeAudio()` |
| `ov2710`, `sgm38121` | PDL camera and camera-power drivers | `initializeCamera()` |
| `lr2021` | RadioLib `LR2021 *` | `initializeLora()` |
| `display_panel`, `dsi_bus`, `display_io` | ESP-IDF LCD handles | `initializeDisplay()` |
| `audio_codec`, `i2s_tx`, `i2s_rx` | ESP-IDF codec and I2S handles | `initializeAudio()`; RX requires capture |
| `sd_card` | `sdmmc_card_t *` | `initializeSdCard()` |

Uninitialized resources are `nullptr`; calling `devices()` does not initialize
anything. The snapshot does not update itself. Call `board.devices()` again
after initialization, deinitialization, reconfiguration, or sleep/wake. Do not
use saved pointers or handles after their resource or Board has been destroyed.
The caller must serialize direct device operations against BSP operations,
including teardown and background tasks that access the same device. The Board
retains ownership: do not delete returned pointers or initialize/deinitialize
the drivers directly. Use Board APIs for lifecycle changes, camera streaming,
and board-controlled power/reset signals.

`Es8389Device` is a BSP adapter over the existing `esp_codec_dev` instance,
sharing its I2C device without allocating another transport or reinitializing
the chip. Its operations return `esp_err_t`. It exposes `readChipId`,
`readRegister`, `writeRegister`, `setVolumeDb`, `volumeDb`, `setMuted`,
`isMuted`, `setMicrophoneGain`, and `isInitialized`. Register operations accept
a positive timeout in milliseconds (at most `INT_MAX`); `readChipId` applies
that timeout to each of its two register reads. Volume, mute, and gain setters
use Board APIs to keep BSP settings synchronized. Raw `writeRegister` calls
bypass those settings; prefer the named setters for normal audio control.

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
LoRa power, and touch-controller reset. Pin constants are declared in
`bsp/pins.hpp`.

## Power modes

The BSP exposes ship, light-sleep, and deep-sleep entry points. Ship mode
uses the AXP517 to cut the battery BATFET. The board can remain powered when
external VBUS is present.

```cpp
ESP_ERROR_CHECK(board.enterShipMode());
```

Light sleep returns after wakeup. Its default wake source is the board button
on GPIO36, active low. Stop and deinitialize camera, audio, LoRa, and SD-card
operations first; the BSP rejects light sleep while those optional resources
are active. An optional timer can be enabled together with the button.

```cpp
bsp::t_panel_p4_round::LightSleepConfig light_sleep{};
light_sleep.wakeup_gpio = bsp::t_panel_p4_round::pins::button;
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
bsp::t_panel_p4_round::DeepSleepConfig deep_sleep{};
deep_sleep.wakeup_gpio = GPIO_NUM_NC;
deep_sleep.timer_wakeup_us = 60'000'000;
ESP_ERROR_CHECK(board.enterDeepSleep(deep_sleep));
```

Before deep sleep, the BSP shuts down all owned peripherals, drives every
XL9555 pin low, and holds the LCD backlight, ESP32-C5 enable, and I2S output
pins low. `initializeI2cBus()` releases those GPIO holds after a deep-sleep
reset.

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
RGB888, the FreeRTOS backend, two software draw units, ESP32-P4 PPA drawing,
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

#include <bsp/board.hpp>

bsp::t_panel_p4_round::Board board;

bsp::t_panel_p4_round::SdCardConfig config{};
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
turns off the LDO and XL9555-controlled SD power.

Block-device owners such as USB MSC must initialize the card without mounting
FatFS locally:

```cpp
bsp::t_panel_p4_round::SdCardConfig config{};
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
full-duplex channel pair and configures ES8389 through `esp_codec_dev`.
Initialization uses I2C retries, early I2S clocks, chip-ID validation and DVDD
reset readback. It leaves the speaker amplifier off until playback is enabled.
The default format is 48 kHz, 16-bit,
stereo I2S with a 384x MCLK.

```cpp
bsp::t_panel_p4_round::Board board;

bsp::t_panel_p4_round::AudioConfig config{};
config.sample_rate_hz = 48000;
config.word_length = bsp::t_panel_p4_round::AudioWordLength::bits16;
config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
config.output_volume_db = -12.0F;
config.microphone_gain = bsp::t_panel_p4_round::AudioMicrophoneGain::db12_5;
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

Audio initialization leaves the external PA off. `setAudioOutputEnabled(true)`
clears stale TX data while the PA is off, restores codec mute state, enables the
PA, and waits 10 ms before returning. After the final PCM write, stop other TX
writers and call `drainAudio()` before `setSpeakerEnabled(false)` so the tail is
audible. For cancellation or errors, disable the PA first. `drainAudio()` has a
bounded timeout (1000 ms by default) and does not change codec mute or PA state.
Music playback manages this sequence automatically, including pause/resume and
failure cleanup. The 10 ms PA settling time should be verified on the board.

`audioCodec()` exposes the initialized `esp_codec_dev` handle. Runtime volume,
mute, and microphone-gain changes use the BSP controls. `audioTransmitChannel()` and
`audioReceiveChannel()` expose the ESP-IDF handles for applications that need
nonblocking I2S callbacks or direct channel operations. ES8389 uses 7-bit I2C
address `0x10`; the `0x20` value in the original Espressif codec header is an
8-bit wire address and must not be passed directly to `I2cDevice`.

On host-only resets, startup drives C5 EN and the I2S outputs low before
cycling XL9555 P0_2, then releases C5 EN after the supply settling delay.
P0_2 controls `LOW_PWR_EN` on the V0.3 schematic; it is unconnected on the
older Round V0.1 schematic, so toggling it cannot power-cycle that revision.
The expander readback verifies the control pin, not the supply voltage.

Audio startup supplies I2S clocks before touching the codec, asserts and
releases DVDD reset/isolation (`F3=01`, `F3=00`), and verifies a reset-register write
before invoking the vendor initialization sequence. ID registers FD/FE
belong to PVDD and do not establish DVDD readiness. Isolation/reset writes
have a startup settling delay; failures report the register, value and
underlying ESP-IDF I2C error. Runtime writes are not automatically retried.

The BSP also provides `reconfigureAudio()` for a controlled runtime format
change. It stops and destroys the active file player before rebuilding the I2S
and codec path, so callers can switch sample rate, word length, slot mode, DMA
settings, or capture mode without racing the playback callback. Runtime
controls include `setAudioVolumeDb()`, `setAudioMuted()`,
`setAudioOutputEnabled()`, `setAudioCaptureEnabled()`,
`setAudioCaptureMuted()`, and `setMicrophoneGain()`. Capture mute is programmed and verified in the
ES8389 ADC; successful muted reads are also zero-filled in software.
Microphone gain is programmed in the ES8389.

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
    const bsp::t_panel_p4_round::RecordingEvent &event,
    void *)
{
    if (event.type == bsp::t_panel_p4_round::RecordingEventType::data) {
        // Consume or copy event.data/event.data_size here.
    }
}

ESP_ERROR_CHECK(board.setRecordingEventCallback(onRecordingEvent));
bsp::t_panel_p4_round::RecordingConfig recording{};
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

## LR2021 LoRa

The BSP owns LR2021 power, the ESP-IDF SPI2 transport, RadioLib HAL, module
objects, GPIO5 interrupt route, and DIO6/DIO7 RF switch table. Initialize the
board before initializing the radio:

```cpp
bsp::t_panel_p4_round::Board board;

bsp::t_panel_p4_round::LoraConfig config{};
config.frequency_mhz = 868.0F;
config.bandwidth_khz = 125.0F;
config.spreading_factor = 7;
config.coding_rate = 5;
config.output_power_dbm = 22;
ESP_ERROR_CHECK(board.initializeLora(config));

LR2021 *radio = board.loraRadio();
ESP_ERROR_CHECK(radio != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);

const std::int16_t state = radio->transmit("Hello from T-Panel-P4");
if (state != RADIOLIB_ERR_NONE) {
    // Handle the RadioLib status here.
}
```

The raw RadioLib status from the last BSP radio operation is available
through `loraLastError()`.

The default configuration targets the sub-GHz path. For 2.4 GHz operation,
set an LR2021-supported high-band frequency, bandwidth, and output power in
`LoraConfig`. RadioLib 7.7.1 accepts -9..22 dBm on the sub-GHz PA and
-19..12 dBm on the high-frequency PA. Applications remain responsible for
regional frequency and transmit-power regulations.
