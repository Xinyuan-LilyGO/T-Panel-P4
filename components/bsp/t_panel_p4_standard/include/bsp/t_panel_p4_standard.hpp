#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <driver/i2s_std.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <sd_pwr_ctrl.h>
#include <sdmmc_cmd.h>
#include <RadioLib.h>

#include <pdl/drivers/camera/ov2710/ov2710.hpp>
#include <pdl/drivers/charge/axp517/axp517.hpp>
#include <pdl/drivers/expand_io/xl9555.hpp>
#include <pdl/drivers/touch/jadard/jadard.hpp>
#include <pdl/ports/esp_idf/gpio_interrupt.hpp>
#include <pdl/ports/esp_idf/i2c_bus.hpp>
#include <pdl/ports/esp_idf/i2c_device.hpp>
#include <pdl/ports/esp_idf/spi_bus.hpp>
#include <pdl/ports/esp_idf/task_delay.hpp>

#include <bsp/t_panel_p4_standard_pins.hpp>

namespace bsp::t_panel_p4_standard
{
struct CameraState;
struct MusicRuntime;

struct DisplayConfig
{
    std::uint8_t frame_buffer_count{2};
    bool read_display_id{true};
};

struct TouchConfig
{
    bool perform_hardware_reset{true};
    bool perform_software_reset{true};
};

struct PowerConfig
{
    bool enable_adc{true};
    bool enable_fuel_gauge{true};
    bool enable_battery_detection{true};
    bool enable_interrupt{false};
    std::uint32_t interrupt_mask{0xFFFFFFFFU};
};

enum class PowerEventType : std::uint8_t
{
    interrupt,
    error,
};

struct PowerEvent
{
    PowerEventType type{PowerEventType::interrupt};
    std::uint32_t interrupt_status{};
    pdl::charge::Axp517PowerStatus power_status{};
    pdl::charge::Axp517FaultStatus fault_status{};
    esp_err_t error{ESP_OK};
};

// Invoked from the BSP power task, never from GPIO interrupt context.
using PowerEventCallback = void (*)(const PowerEvent &event, void *context);

enum class ShipModeStep : std::uint8_t
{
    usb_msc,
    sd_card,
    camera,
    display,
    touch,
    audio,
    lora,
    power_interrupt,
    axp517,
    finalize,
};

using ShipModeProgressCallback = void (*)(ShipModeStep step, bool complete,
                                           esp_err_t result, void *context);
using SleepProgressCallback = void (*)(std::uint8_t step, bool complete,
                                      esp_err_t result, void *context);

struct CameraConfig
{
    pdl::camera::Ov2710Mode mode{
        pdl::camera::Ov2710Mode::raw10_1920x1080_25fps};
    std::uint8_t capture_buffer_count{2};
    std::uint8_t auto_exposure_target{0x30};
    std::uint32_t isp_clock_hz{160'000'000};
    bool allow_non_dma_psram_fallback{true};
};

struct CameraFrame
{
    const std::uint8_t *data{};
    std::size_t size{};
};

struct CameraStatistics
{
    std::uint32_t received_frames{};
    std::uint32_t dropped_frames{};
    std::uint32_t recovery_count{};
    std::uint32_t recovery_failures{};
    esp_err_t last_error{ESP_OK};
};

enum class AudioCodecChip : std::uint8_t
{
    es8389,
};

enum class AudioWordLength : std::uint8_t
{
    bits16 = 16,
    bits24 = 24,
    bits32 = 32,
};

enum class AudioMicrophoneChannel : std::uint8_t
{
    left = 0x01,
    right = 0x02,
    both = 0x03,
};

enum class AudioMicrophoneGain : std::uint8_t
{
    db0 = 0x00,
    db3_5 = 0x01,
    db6_5 = 0x02,
    db9_5 = 0x03,
    db12_5 = 0x06,
    db15_5 = 0x07,
    db18_5 = 0x08,
    db21_5 = 0x09,
    db24_5 = 0x0A,
    db27_5 = 0x0B,
    db30_5 = 0x0C,
    db33_5 = 0x0D,
    db36_5 = 0x0E,
};

struct AudioConfig
{
    AudioCodecChip codec{AudioCodecChip::es8389};
    std::uint32_t sample_rate_hz{48000};
    AudioWordLength word_length{AudioWordLength::bits16};
    i2s_mclk_multiple_t mclk_multiple{I2S_MCLK_MULTIPLE_384};
    i2s_slot_mode_t slot_mode{I2S_SLOT_MODE_STEREO};
    i2s_std_slot_mask_t slot_mask{I2S_STD_SLOT_BOTH};
    int dma_descriptor_count{8};
    int dma_frames_per_descriptor{512};
    float output_volume_db{0.0F};
    AudioMicrophoneGain microphone_gain{AudioMicrophoneGain::db12_5};
    bool no_dac_reference{false};
    bool enable_capture{true};
    bool enable_speaker{true};
};

struct AudioStatistics
{
    std::uint32_t tx_calls{};
    std::uint32_t rx_calls{};
    std::uint32_t tx_bytes{};
    std::uint32_t rx_bytes{};
    std::uint32_t tx_timeouts{};
    std::uint32_t rx_timeouts{};
    std::uint32_t tx_errors{};
    std::uint32_t rx_errors{};
    std::uint32_t tx_short_writes{};
    std::uint32_t rx_short_reads{};
};

enum class RecordingState : std::uint8_t
{
    idle,
    starting,
    recording,
    stopping,
    stopped,
    error,
};

enum class RecordingEventType : std::uint8_t
{
    state_changed,
    data,
    error,
};

struct RecordingEvent
{
    RecordingEventType type{RecordingEventType::state_changed};
    RecordingState state{RecordingState::idle};
    esp_err_t error{ESP_OK};
    std::uint32_t bytes_recorded{};
    std::uint32_t duration_ms{};
    // Valid only while the callback is executing; copy it if it must be kept.
    const std::uint8_t *data{};
    std::size_t data_size{};
};

using RecordingEventCallback = void (*)(const RecordingEvent &event, void *context);
// Keep callbacks short; do not call startRecording()/stopRecording() from one.

struct RecordingConfig
{
    const char *path{};
    std::size_t ring_buffer_bytes{64U * 1024U};
    std::size_t chunk_bytes{4096U};
    std::uint32_t read_timeout_ms{100U};
};

enum class MusicCodec : std::uint8_t
{
    unknown,
    mp3,
    wav,
    aac,
    flac,
    m4a,
    opus,
    ts,
};

struct MusicFormat
{
    MusicCodec codec{MusicCodec::unknown};
    std::int32_t sample_rate_hz{};
    std::int32_t bitrate_bps{};
    std::uint8_t channels{};
    std::uint8_t bits_per_sample{};
};

struct MusicProbe
{
    MusicFormat format{};
    std::uint32_t duration_ms{};
    bool has_id3{};
};

struct MusicPlaybackConfig
{
    bool auto_configure_audio{true};
    bool allow_resample{false};
    bool reject_unknown_format{false};
};

enum class MusicState : std::uint8_t
{
    idle,
    starting,
    playing,
    paused,
    stopped,
    finished,
    error,
};

enum class MusicEventType : std::uint8_t
{
    state_changed,
    music_info,
    error,
};

struct MusicEvent
{
    MusicEventType type{MusicEventType::state_changed};
    MusicState state{MusicState::idle};
    MusicFormat format{};
    std::uint32_t position_ms{};
    std::uint32_t duration_ms{};
    esp_err_t error{ESP_OK};
    std::array<char, 192> path{};
};

using MusicEventCallback = void (*)(const MusicEvent &event, void *context);

// Called from the p4_music task when a track finishes. The callback must only
// provide the next path; it must not call back into Board music APIs.
using MusicNextTrackCallback = bool (*)(char *path, std::size_t path_size,
                                        void *context);

struct MusicStatistics
{
    std::uint32_t output_calls{};
    std::uint32_t output_bytes{};
    std::uint32_t output_short_writes{};
    std::uint32_t output_errors{};
    std::uint32_t playback_timeouts{};
    std::uint32_t command_timeouts{};
    std::uint32_t task_stack_high_watermark_words{};
};

struct LoraConfig
{
    std::uint32_t spi_frequency_hz{2000000};
    float frequency_mhz{868.0F};
    float bandwidth_khz{125.0F};
    std::uint8_t spreading_factor{7};
    std::uint8_t coding_rate{5};
    std::uint8_t sync_word{0x12};
    std::int8_t output_power_dbm{22};
    std::uint16_t preamble_length{12};
    float tcxo_voltage{3.3F};
    bool crc_enabled{true};
    bool invert_iq{false};
    std::uint32_t power_up_delay_ms{10};
};

struct SdCardConfig
{
    const char *mount_point{"/sdcard"};
    std::size_t maximum_open_files{5};
    std::size_t allocation_unit_size{16U * 1024U};
    std::uint32_t maximum_frequency_khz{20000};
    std::uint32_t power_up_delay_ms{200};
    bool format_if_mount_failed{false};
    bool enable_internal_pullups{true};
    // Disable filesystem mounting when a block-device owner such as USB MSC
    // needs exclusive access to the raw SD/MMC card.
    bool mount_filesystem{true};
};

struct LightSleepConfig
{
    gpio_num_t wakeup_gpio{pins::button};
    bool wakeup_on_high{false};
    std::uint64_t timer_wakeup_us{};
    bool suspend_display{true};
};

struct DeepSleepConfig
{
    // ESP32-P4 deep-sleep GPIO wakeup is limited to GPIO0..GPIO15.
    // GPIO_NUM_NC leaves GPIO wakeup disabled; reset or another configured
    // source is then required when timer_wakeup_us is also zero.
    gpio_num_t wakeup_gpio{GPIO_NUM_NC};
    bool wakeup_on_high{false};
    std::uint64_t timer_wakeup_us{};
};

class Board
{
public:
    static constexpr int displayWidth = 720;
    static constexpr int displayHeight = 720;
    static constexpr std::uint8_t displayBitsPerPixel = 24;
    static constexpr std::uint8_t maximumFrameBufferCount = 3;
    static constexpr std::uint8_t maximumCameraBufferCount = 3;

    Board();
    ~Board();

    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;

    [[nodiscard]] esp_err_t initialize();
    [[nodiscard]] esp_err_t deinitialize();
    [[nodiscard]] bool isInitialized() const noexcept;

    // Ship mode cuts the AXP517 battery BATFET and may remove power before
    // this function returns. External VBUS can keep the system powered.
    [[nodiscard]] esp_err_t enterShipMode(
        std::uint32_t timeout_ms = 100,
        ShipModeProgressCallback progress_callback = nullptr,
        void *progress_context = nullptr);
    // Enter and return from ESP32-P4 light sleep. Display and peripheral
    // state are left untouched.
    [[nodiscard]] esp_err_t enterLightSleep();
    [[nodiscard]] esp_err_t enterLightSleep(const LightSleepConfig &config,
        SleepProgressCallback progress = nullptr, void *context = nullptr);
    // On success deep sleep does not return. With no wake source configured,
    // the board requires reset or a power cycle to boot again.
    [[nodiscard]] esp_err_t enterDeepSleep();
    [[nodiscard]] esp_err_t enterDeepSleep(const DeepSleepConfig &config,
        SleepProgressCallback progress = nullptr, void *context = nullptr);

    [[nodiscard]] esp_err_t initializeDisplay();
    [[nodiscard]] esp_err_t initializeDisplay(const DisplayConfig &config);
    [[nodiscard]] esp_err_t deinitializeDisplay();
    [[nodiscard]] bool isDisplayInitialized() const noexcept;

    [[nodiscard]] esp_err_t initializeTouch();
    [[nodiscard]] esp_err_t initializeTouch(const TouchConfig &config);
    [[nodiscard]] esp_err_t deinitializeTouch();
    [[nodiscard]] bool isTouchInitialized() const noexcept;
    [[nodiscard]] esp_err_t initializeTouchInterrupt(
        pdl::esp_idf::GpioInterrupt::IsrHandler handler,
        void *argument = nullptr,
        gpio_int_type_t trigger = GPIO_INTR_NEGEDGE);
    [[nodiscard]] esp_err_t deinitializeTouchInterrupt();

    [[nodiscard]] esp_err_t initializePower();
    [[nodiscard]] esp_err_t initializePower(const PowerConfig &config);
    [[nodiscard]] esp_err_t deinitializePower();
    [[nodiscard]] bool isPowerInitialized() const noexcept;
    [[nodiscard]] esp_err_t setPowerEventCallback(
        PowerEventCallback callback,
        void *context = nullptr);
    [[nodiscard]] esp_err_t readPowerStatus(
        pdl::charge::Axp517PowerStatus &status,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t setChargerEnabled(
        bool enabled,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t getChargerEnabled(
        bool &enabled,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t readPowerFaults(
        pdl::charge::Axp517FaultStatus &status,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t clearPowerFaults(
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t readBatteryPercentage(
        std::uint8_t &percent,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t readBatteryVoltageMv(
        std::uint16_t &millivolts,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t readBatteryCurrentMa(
        std::int16_t &milliamps,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t readVbusVoltageMv(
        std::uint16_t &millivolts,
        std::uint32_t timeout_ms = 100);

    [[nodiscard]] esp_err_t initializeCamera();
    [[nodiscard]] esp_err_t initializeCamera(const CameraConfig &config);
    [[nodiscard]] esp_err_t deinitializeCamera();
    [[nodiscard]] bool isCameraInitialized() const noexcept;
    [[nodiscard]] bool isCameraStreaming() const noexcept;
    [[nodiscard]] CameraConfig cameraConfig() const noexcept;
    [[nodiscard]] esp_err_t startCameraStreaming();
    [[nodiscard]] esp_err_t stopCameraStreaming();
    [[nodiscard]] esp_err_t reconfigureCamera(const CameraConfig &config);
    [[nodiscard]] esp_err_t recoverCameraPipeline();
    [[nodiscard]] esp_err_t readCameraProductId(
        std::uint16_t &product_id,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t softwareResetCameraSensor(
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t setCameraTestPattern(
        bool enabled,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t setCameraHorizontalMirror(
        bool enabled,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t setCameraVerticalFlip(
        bool enabled,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t setCameraAutoExposureTarget(
        std::uint8_t target,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t readCameraSensorStatistics(
        pdl::camera::Ov2710Statistics &statistics,
        std::uint32_t timeout_ms = 100);
    [[nodiscard]] esp_err_t acquireCameraFrame(
        CameraFrame &frame,
        std::uint32_t timeout_ms = 2000);
    [[nodiscard]] esp_err_t releaseCameraFrame(const std::uint8_t *frame);
    [[nodiscard]] pdl::camera::Ov2710ModeInfo cameraModeInfo() const noexcept;
    [[nodiscard]] CameraStatistics cameraStatistics() const noexcept;

    [[nodiscard]] esp_err_t initializeAudio();
    [[nodiscard]] esp_err_t initializeAudio(const AudioConfig &config);
    [[nodiscard]] esp_err_t reconfigureAudio(const AudioConfig &config);
    [[nodiscard]] esp_err_t deinitializeAudio();
    [[nodiscard]] bool isAudioInitialized() const noexcept;
    [[nodiscard]] AudioConfig audioConfig() const noexcept;
    [[nodiscard]] AudioStatistics audioStatistics() const noexcept;
    void resetAudioStatistics() noexcept;
    [[nodiscard]] esp_err_t readAudio(
        void *data,
        std::size_t size,
        std::size_t &bytes_read,
        std::uint32_t timeout_ms = 1000);
    [[nodiscard]] esp_err_t writeAudio(
        const void *data,
        std::size_t size,
        std::size_t &bytes_written,
        std::uint32_t timeout_ms = 1000);
    [[nodiscard]] esp_err_t setAudioVolumeDb(float decibels);
    [[nodiscard]] float audioVolumeDb() const noexcept;
    [[nodiscard]] esp_err_t setAudioMuted(bool muted);
    [[nodiscard]] bool isAudioMuted() const noexcept;
    [[nodiscard]] esp_err_t setAudioOutputEnabled(bool enabled);
    [[nodiscard]] bool isAudioOutputEnabled() const noexcept;
    [[nodiscard]] esp_err_t setAudioCaptureEnabled(bool enabled);
    [[nodiscard]] bool isAudioCaptureEnabled() const noexcept;
    [[nodiscard]] esp_err_t setAudioCaptureMuted(bool muted);
    [[nodiscard]] bool isAudioCaptureMuted() const noexcept;
    [[nodiscard]] esp_err_t setMicrophoneGain(
        AudioMicrophoneGain gain,
        AudioMicrophoneChannel channels = AudioMicrophoneChannel::both);

    [[nodiscard]] esp_err_t startRecording(const char *path);
    [[nodiscard]] esp_err_t startRecording(const RecordingConfig &config);
    [[nodiscard]] esp_err_t stopRecording(std::uint32_t timeout_ms = 2000);
    [[nodiscard]] esp_err_t setRecordingEventCallback(
        RecordingEventCallback callback,
        void *context = nullptr);
    [[nodiscard]] RecordingState recordingState() const noexcept;
    [[nodiscard]] esp_err_t recordingLastError() const noexcept;
    [[nodiscard]] std::uint32_t recordingBytes() const noexcept;
    [[nodiscard]] std::uint32_t recordingDurationMs() const noexcept;
    [[nodiscard]] bool isRecording() const noexcept;

    [[nodiscard]] esp_err_t playMusicFile(const char *path);
    [[nodiscard]] esp_err_t playMusicFile(
        const char *path,
        const MusicPlaybackConfig &config);
    [[nodiscard]] esp_err_t probeMusicFile(
        const char *path,
        MusicProbe &probe) const;
    [[nodiscard]] esp_err_t pauseMusic(bool paused);
    [[nodiscard]] esp_err_t stopMusic();
    [[nodiscard]] esp_err_t stopMusic(std::uint32_t timeout_ms);
    [[nodiscard]] esp_err_t deinitializeMusic();
    [[nodiscard]] esp_err_t setMusicEventCallback(
        MusicEventCallback callback,
        void *context = nullptr);
    [[nodiscard]] esp_err_t setMusicNextTrackCallback(
        MusicNextTrackCallback callback,
        void *context = nullptr);
    [[nodiscard]] MusicState musicState() const noexcept;
    [[nodiscard]] MusicFormat musicFormat() const noexcept;
    [[nodiscard]] esp_err_t musicLastError() const noexcept;
    [[nodiscard]] const char *musicLastErrorName() const noexcept;
    [[nodiscard]] MusicStatistics musicStatistics() const noexcept;
    void resetMusicStatistics() noexcept;
    [[nodiscard]] bool isMusicPlaying() const noexcept;
    [[nodiscard]] bool isMusicPaused() const noexcept;
    [[nodiscard]] std::uint32_t musicPositionMs() const noexcept;
    [[nodiscard]] std::uint32_t musicDurationMs() const noexcept;
    [[nodiscard]] std::uint8_t musicLevelPercent() const noexcept;
    void getCurrentMusicPath(char *path, std::size_t size) const noexcept;
    void getMusicWaveform(std::int16_t *samples, std::size_t count) const noexcept;
    [[nodiscard]] esp_err_t seekMusicMs(std::uint32_t position_ms);
    [[nodiscard]] esp_err_t setMusicVolumePercent(std::int32_t percent);
    [[nodiscard]] esp_err_t fadeMusicVolumePercent(
        std::int32_t target_percent,
        std::uint32_t duration_ms);
    [[nodiscard]] std::int32_t musicVolumePercent() const noexcept;
    [[nodiscard]] esp_err_t getMusicCover(std::vector<std::uint8_t> &cover) const;
    [[nodiscard]] esp_err_t getMusicLyrics(std::string &lyrics) const;
    [[nodiscard]] esp_err_t getMusicCover(const char *path, std::vector<std::uint8_t> &cover) const;
    [[nodiscard]] esp_err_t getMusicLyrics(const char *path, std::string &lyrics) const;
    [[nodiscard]] esp_err_t getMusicMetadata(
        const char *path, std::vector<std::uint8_t> &cover, std::string &lyrics) const;

    [[nodiscard]] esp_err_t initializeLora();
    [[nodiscard]] esp_err_t initializeLora(const LoraConfig &config);
    [[nodiscard]] esp_err_t deinitializeLora();
    [[nodiscard]] bool isLoraInitialized() const noexcept;
    [[nodiscard]] esp_err_t loraStandby();
    [[nodiscard]] esp_err_t loraSleep();
    [[nodiscard]] std::int16_t loraLastError() const noexcept;

    [[nodiscard]] esp_err_t initializeSdCard();
    [[nodiscard]] esp_err_t initializeSdCard(const SdCardConfig &config);
    [[nodiscard]] esp_err_t deinitializeSdCard();
    [[nodiscard]] bool isSdCardInitialized() const noexcept;
    [[nodiscard]] bool isSdCardMounted() const noexcept;
    [[nodiscard]] esp_err_t initializeUsbMsc();
    [[nodiscard]] esp_err_t deinitializeUsbMsc();
    [[nodiscard]] bool isUsbMscInitialized() const noexcept;

    [[nodiscard]] esp_err_t setBacklightBrightness(std::uint8_t percent);
    [[nodiscard]] std::uint8_t backlightBrightness() const noexcept;
    // Light-sleep display control: suspend the DPI stream/GDMA around P4 light
    // sleep, resynchronize JD9365 with write-only commands, and toggle the
    // backlight. Resume performs no panel register reads.
    [[nodiscard]] esp_err_t setDisplaySleep(bool sleeping);

    [[nodiscard]] esp_err_t setSpeakerEnabled(bool enabled);
    [[nodiscard]] bool isSpeakerEnabled() const noexcept;
    [[nodiscard]] esp_err_t setSdPowerEnabled(bool enabled);
    [[nodiscard]] esp_err_t setLoraPowerEnabled(bool enabled);
    [[nodiscard]] esp_err_t setC5AudioLcdEnabled(bool enabled);
    [[nodiscard]] bool isC5AudioLcdEnabled() const noexcept;
    [[nodiscard]] esp_err_t resetTouchController();

    [[nodiscard]] pdl::esp_idf::I2cBus *i2cBus() noexcept;
    [[nodiscard]] pdl::esp_idf::SpiBus *spiBus() noexcept;
    [[nodiscard]] pdl::expand_io::Xl9555 *ioExpander() noexcept;
    [[nodiscard]] pdl::touch::JadardTouch *touchController() noexcept;
    [[nodiscard]] esp_codec_dev_handle_t audioCodec() noexcept;
    [[nodiscard]] i2s_chan_handle_t audioTransmitChannel() const noexcept;
    [[nodiscard]] i2s_chan_handle_t audioReceiveChannel() const noexcept;
    [[nodiscard]] LR2021 *loraRadio() noexcept;
    [[nodiscard]] const LoraConfig *loraConfig() const noexcept;
    [[nodiscard]] sdmmc_card_t *sdCard() noexcept;
    [[nodiscard]] const char *sdCardMountPoint() const noexcept;
    [[nodiscard]] esp_lcd_panel_handle_t displayPanel() const noexcept;
    [[nodiscard]] void *frameBuffer(std::uint8_t index = 0) const noexcept;
    [[nodiscard]] std::uint8_t frameBufferCount() const noexcept;

private:
    [[nodiscard]] MusicRuntime *ensureMusicRuntime();
    static void recordingCaptureTaskEntry(void *argument);
    static void recordingWriterTaskEntry(void *argument);
    void notifyRecordingEvent(
        RecordingEventType type,
        RecordingState state,
        esp_err_t error = ESP_OK,
        const std::uint8_t *data = nullptr,
        std::size_t data_size = 0U);
    [[nodiscard]] esp_err_t initializeBacklight();
    [[nodiscard]] esp_err_t setExpanderOutput(
        std::uint8_t pin,
        bool high);
    [[nodiscard]] esp_err_t assertDisplayReset();
    [[nodiscard]] esp_err_t releaseDisplayReset();
    [[nodiscard]] esp_err_t prepareForPowerDown(
        ShipModeProgressCallback progress_callback = nullptr,
        void *progress_context = nullptr);
    static void powerInterruptHandler(void *argument);
    static void powerEventTaskEntry(void *argument);
    void stopPowerEventTask() noexcept;

    pdl::esp_idf::I2cBus i2c_bus_;
    pdl::esp_idf::SpiBus spi_bus_;
    std::optional<pdl::esp_idf::I2cDevice> xl9555_transport_;
    std::optional<pdl::expand_io::Xl9555> xl9555_;
    std::optional<pdl::esp_idf::I2cDevice> touch_transport_;
    std::optional<pdl::touch::JadardTouch> touch_;
    bool touch_perform_hardware_reset_{true};
    bool touch_perform_software_reset_{true};
    pdl::esp_idf::GpioInterrupt touch_interrupt_;
    std::optional<pdl::esp_idf::I2cDevice> axp517_transport_;
    std::optional<pdl::charge::Axp517> axp517_;
    pdl::esp_idf::GpioInterrupt power_interrupt_;
    TaskHandle_t power_event_task_{};
    SemaphoreHandle_t power_event_stopped_{};
    std::atomic<bool> power_task_stop_{false};
    std::atomic<PowerEventCallback> power_event_callback_{nullptr};
    std::atomic<void *> power_event_context_{nullptr};
    PowerConfig power_config_{};
    bool power_ship_mode_{};
    pdl::esp_idf::TaskDelay task_delay_;
    const audio_codec_data_if_t *audio_data_if_{};
    const audio_codec_ctrl_if_t *audio_ctrl_if_{};
    const audio_codec_gpio_if_t *audio_gpio_if_{};
    const audio_codec_if_t *audio_codec_if_{};
    esp_codec_dev_handle_t audio_codec_{};
    i2s_chan_handle_t audio_tx_{};
    i2s_chan_handle_t audio_rx_{};
    std::unique_ptr<CameraState> camera_state_;
    std::unique_ptr<RadioLibHal> lora_hal_;
    std::unique_ptr<Module> lora_module_;
    std::unique_ptr<LR2021> lora_radio_;
    std::optional<LoraConfig> lora_config_;
    sdmmc_card_t *sd_card_{};
    std::unique_ptr<sdmmc_card_t> raw_sd_card_;
    sdmmc_host_t sd_host_{};
    sd_pwr_ctrl_handle_t sd_power_control_{};
    std::array<char, 64> sd_mount_point_{};
    SdCardConfig sd_card_config_{};
    std::array<char, 64> sd_card_config_mount_point_{};
    bool sd_host_initialized_{};
    bool sd_card_mounted_{};
    void *usb_msc_storage_{};
    bool usb_driver_initialized_{};
    bool usb_msc_driver_initialized_{};
    bool usb_msc_owns_sd_card_{};

    esp_ldo_channel_handle_t mipi_phy_ldo_{};
    esp_lcd_dsi_bus_handle_t dsi_bus_{};
    esp_lcd_panel_io_handle_t dbi_io_{};
    esp_lcd_panel_handle_t panel_{};
    std::array<void *, maximumFrameBufferCount> frame_buffers_{};

    std::uint8_t frame_buffer_count_{};
    bool display_read_display_id_{true};
    std::uint8_t backlight_brightness_{};
    std::uint8_t display_sleep_brightness_{};
    bool display_sleeping_{};
    bool initialized_{};
    bool backlight_initialized_{};
    bool panel_initialized_{};
    bool display_initialized_{};
    bool audio_initialized_{};
    bool audio_tx_enabled_{};
    bool audio_rx_enabled_{};
    bool audio_capture_enabled_{};
    bool audio_capture_muted_{};
    bool speaker_enabled_{};
    bool c5_audio_lcd_enabled_{};
    bool audio_output_enabled_{};
    bool audio_output_muted_{};
    MusicRuntime *music_runtime_{};
    AudioConfig audio_config_{};
    std::atomic<float> audio_volume_db_{0.0F};
    std::atomic<std::uint32_t> audio_tx_calls_{};
    std::atomic<std::uint32_t> audio_rx_calls_{};
    std::atomic<std::uint32_t> audio_tx_bytes_{};
    std::atomic<std::uint32_t> audio_rx_bytes_{};
    std::atomic<std::uint32_t> audio_tx_timeouts_{};
    std::atomic<std::uint32_t> audio_rx_timeouts_{};
    std::atomic<std::uint32_t> audio_tx_errors_{};
    std::atomic<std::uint32_t> audio_rx_errors_{};
    std::atomic<std::uint32_t> audio_tx_short_writes_{};
    std::atomic<std::uint32_t> audio_rx_short_reads_{};
    std::atomic<RecordingState> recording_state_{RecordingState::idle};
    std::atomic<esp_err_t> recording_last_error_{ESP_OK};
    std::atomic<std::uint32_t> recording_bytes_{};
    std::atomic<std::int64_t> recording_started_us_{};
    std::atomic<bool> recording_stop_requested_{false};
    std::atomic<bool> recording_capture_done_{false};
    std::atomic<RecordingEventCallback> recording_callback_{nullptr};
    std::atomic<void *> recording_callback_context_{nullptr};
    TaskHandle_t recording_capture_task_{};
    TaskHandle_t recording_writer_task_{};
    StreamBufferHandle_t recording_stream_buffer_{};
    SemaphoreHandle_t recording_done_{};
    FILE *recording_file_{};
    std::array<char, 192> recording_path_{};
    std::size_t recording_chunk_bytes_{};
    std::uint32_t recording_read_timeout_ms_{};
    std::uint32_t recording_sample_rate_{};
    std::uint16_t recording_channels_{};
    std::uint16_t recording_bits_per_sample_{};
    bool lora_initialized_{};
    std::int16_t lora_last_error_{};
};
}
