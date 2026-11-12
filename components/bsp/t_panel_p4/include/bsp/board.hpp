#pragma once

/**
 * @file board.hpp
 * @brief T-Panel-P4 板级配置、设备视图与统一硬件 API。
 *
 * 分层：hardware.hpp 选择板型；pins.hpp 描述接线；本文件声明配置与 API；
 * src/ 下各外设 .cpp 实现驱动调用、硬件时序与资源管理。
 *
 * 硬件差异：
 * - Standard：720x720 JD9365 + Jadard，ES8389 录放音，XL9555 / LR2021。
 * - Round：720x720 JD9365DA + GT911，ES8389 录放音，XL9555 / LR2021。
 * - Rect：720x1440 ILI9882 显示/触摸，ES8389 播放 + ES7210 采集。
 *   Rect 无 XL9555 / LR2021，控制引脚直接连接 ESP32-P4 GPIO。
 *
 * 阅读顺序：配置与数据类型 -> 借用设备视图 -> 公共 API -> 私有实现与资源。
 * 未标注板型的配置/API 适用于三种板型；引脚数值以 bsp::pins 为准。
 */

#include <bsp/hardware.hpp>
#include <bsp/pins.hpp>
#include <bsp/touch.hpp>

// C++ 容器与原子状态。
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ESP-IDF 外设句柄、FreeRTOS 与存储接口。
#include <esp_err.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <driver/i2s_std.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <hal/cam_ctlr_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <sd_pwr_ctrl.h>
#include <sdmmc_cmd.h>

// 共用芯片驱动及 ESP-IDF 传输适配器。
#include <pdl/drivers/camera/ov2710/ov2710.hpp>
#include <pdl/drivers/charge/axp517/axp517.hpp>
#include <pdl/drivers/power/sgm38121/sgm38121.hpp>

#include <pdl/ports/esp_idf/gpio_interrupt.hpp>
#include <pdl/ports/esp_idf/i2c_bus.hpp>
#include <pdl/ports/esp_idf/i2c_device.hpp>
#include <pdl/ports/esp_idf/task_delay.hpp>

#if !T_PANEL_P4_BSP_IS_RECT
// 仅 Standard / Round：SPI、IO 扩展与 LoRa。
#include <RadioLib.h>
#include <pdl/ports/esp_idf/spi_bus.hpp>
#include <pdl/drivers/expand_io/xl9555.hpp>
#endif

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    // 不完整类型：内部状态定义在对应 .cpp，避免暴露管线/播放器实现。
    struct CameraState;
    struct MusicRuntime;

    // ========================================================================
    // 1.1 显示配置：MIPI DSI / DPI；src/display.cpp
    // ========================================================================
    struct DisplayConfig
    {
        std::uint8_t frame_buffer_count{2};
        // Rect's DCS ID read can leave ESP-IDF waiting forever for RX data.
        // Keep it opt-in on that panel; initialization does not need the ID.
        bool read_display_id{!T_PANEL_P4_BSP_IS_RECT};
        // Format stored in frameBuffer() and supplied to draw_bitmap().
        lcd_color_format_t in_color_format{LCD_COLOR_FMT_RGB888};
        // Panel/DSI output depth, independent of the framebuffer input format.
        std::uint8_t bits_per_pixel{24};
    };

    // ========================================================================
    // 1.2 触摸配置：Standard Jadard / Round GT911 / Rect ILI9882；src/touch.cpp
    // ========================================================================
    struct TouchConfig
    {
        std::uint16_t i2c_address{TouchController::defaultI2cAddress};
        std::uint32_t i2c_frequency_hz{TouchController::defaultI2cFrequencyHz};
        bool perform_hardware_reset{true};
        bool perform_software_reset{false};
        gpio_int_type_t interrupt_trigger{T_PANEL_P4_BSP_IS_RECT ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE};
        // ISR context: notify a task; do not read I2C here. Null permits polling.
        pdl::esp_idf::GpioInterrupt::IsrHandler interrupt_handler{};
        void *interrupt_argument{};
        // Round only: if the requested GT911 address NACKs, try the other
        // supported address once. touchConfig() reports the address in use.
        bool try_alternate_i2c_address{true};
    };

    // ========================================================================
    // 1.3 电源配置与事件：AXP517；src/power.cpp
    // ========================================================================
    struct PowerConfig
    {
        bool enable_adc{true};
        bool enable_fuel_gauge{true};
        bool enable_battery_detection{true};
        bool enable_interrupt{false};
        std::uint32_t interrupt_mask{0xFFFFFFFFU};
        // Single-cell Li-ion defaults, shared by all hardware versions.
        std::uint16_t input_current_limit_ma{1500};
        std::uint16_t input_voltage_limit_mv{4600};
        std::uint16_t minimum_system_voltage_mv{3500};
        std::uint16_t charge_voltage_mv{4200};
        std::uint16_t charge_current_ma{512};
        std::uint16_t precharge_current_ma{128};
        std::uint16_t trickle_current_ma{64};
        std::uint16_t termination_current_ma{128};
        bool enable_charge_termination{true};
        bool enable_charger{true};
        bool enable_bc12_detection{true};
        bool enable_type_c_detection{true};
        bool enable_charge_led{true};
        pdl::charge::Axp517ChargeLedOutputType charge_led_output_type{
            pdl::charge::Axp517ChargeLedOutputType::open_drain};
        pdl::charge::Axp517ChargeLedMode charge_led_mode{
            pdl::charge::Axp517ChargeLedMode::type_a};
        pdl::charge::Axp517ChargeLedCfgState charge_led_cfg_state{
            pdl::charge::Axp517ChargeLedCfgState::high_impedance};
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

    // ========================================================================
    // 1.4 摄像头配置、帧与统计：OV2710 / CSI / ISP；src/camera.cpp
    // ========================================================================
    struct CameraConfig
    {
        pdl::camera::Ov2710Mode mode{pdl::camera::Ov2710Mode::raw10_1920x1080_25fps};
        // Three buffers separate application reads, the latest frame, and DMA capture.
        // Two buffers are supported to reduce PSRAM use, at the cost of more dropped frames.
        std::uint8_t capture_buffer_count{3};
        std::uint8_t auto_exposure_target{0x30};
        std::uint32_t isp_clock_hz{160'000'000};
        bool allow_non_dma_psram_fallback{true};
        // Sensor/CSI input must match the selected OV2710 mode (currently RAW10).
        cam_ctlr_color_t input_data_color_type{CAM_CTLR_COLOR_RAW10};
        // ISP/CSI output stored in capture buffers: RGB565 or packed RGB888.
        cam_ctlr_color_t output_data_color_type{CAM_CTLR_COLOR_RGB565};
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

    // ========================================================================
    // 1.5 音频配置与统计：ES8389 / ES7210 / I2S；src/audio.cpp
    // ========================================================================
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

    // ========================================================================
    // 1.6 录音配置与事件：PCM 采集及 WAV 写入；src/recording.cpp
    // ========================================================================
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
        const std::uint8_t *data{};
        std::size_t data_size{};
    };

    using RecordingEventCallback = void (*)(const RecordingEvent &event, void *context);

    struct RecordingConfig
    {
        const char *path{};
        // PCM ring/chunks and recording task stacks are allocated in PSRAM.
        std::size_t ring_buffer_bytes{64U * 1024U};
        std::size_t chunk_bytes{4096U};
        std::uint32_t read_timeout_ms{100U};
    };

    // ========================================================================
    // 1.7 音乐配置与事件：文件播放、格式探测和元数据；src/music.cpp
    // ========================================================================
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
        // With compiled resampling, configure I2S to the destination rate.
        bool auto_configure_audio{true};
        // Without compiled resampling, keep the current I2S rate when true;
        // playback rejects a known source rate that does not match it.
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
    using MusicNextTrackCallback = bool (*)(char *path, std::size_t path_size, void *context);

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

    // ========================================================================
    // 1.8 LoRa 配置：仅 Standard / Round 的 LR2021；src/lora.cpp
    // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
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
#endif

    // ========================================================================
    // 1.9 存储配置：SDMMC / FAT；src/sd_card.cpp
    // ========================================================================
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

    // ========================================================================
    // 1.10 休眠配置：GPIO / 定时器唤醒；src/board.cpp
    // ========================================================================
    // 休眠前释放已初始化的显示/触摸资源，唤醒后按原配置恢复。
    struct LightSleepConfig
    {
        gpio_num_t wakeup_gpio{pins::button};
        bool wakeup_on_high{false};
        std::uint64_t timer_wakeup_us{};
    };

    struct DeepSleepConfig
    {
        // ESP32-P4 deep-sleep GPIO wakeup is limited to GPIO0..GPIO15.
        // GPIO_NUM_NC leaves GPIO wakeup disabled; reset or another configured
        // source is then required when timer_wakeup_us is also zero.
#if !T_PANEL_P4_BSP_IS_RECT
        gpio_num_t wakeup_gpio{GPIO_NUM_NC};
#else
        gpio_num_t wakeup_gpio{pins::button};
#endif
        bool wakeup_on_high{false};
        std::uint64_t timer_wakeup_us{};
    };

    // ========================================================================
    // 2. 设备访问视图：Board 持有资源，调用方仅借用
    // ========================================================================
    class Board;

    // Operates on Board's existing esp_codec_dev instance; owns no hardware.
    class Es8389Device
    {
    public:
        Es8389Device(const Es8389Device &) = delete;
        Es8389Device &operator=(const Es8389Device &) = delete;

        [[nodiscard]] bool isInitialized() const noexcept;
        [[nodiscard]] esp_err_t readChipId(std::uint16_t &chip_id,
                                           std::uint32_t timeout_ms = 100) const;
        [[nodiscard]] esp_err_t readRegister(std::uint8_t address, std::uint8_t &value,
                                             std::uint32_t timeout_ms = 100) const;
        // Raw writes bypass Board's cached audio settings.
        [[nodiscard]] esp_err_t writeRegister(std::uint8_t address, std::uint8_t value,
                                              std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setVolumeDb(float decibels);
        [[nodiscard]] float volumeDb() const noexcept;
        [[nodiscard]] esp_err_t setMuted(bool muted);
        [[nodiscard]] bool isMuted() const noexcept;
        [[nodiscard]] esp_err_t setMicrophoneGain(
            AudioMicrophoneGain gain,
            AudioMicrophoneChannel channels = AudioMicrophoneChannel::both);

    private:
        friend class Board;
        explicit Es8389Device(Board &board) noexcept : board_(board) {}
        Board &board_;
    };

    // Non-owning snapshot. Null means the resource is not initialized.
    // Reacquire after initialization, teardown, reconfiguration or sleep.
    struct Devices
    {
        // 共享总线与板级 IO。
        pdl::esp_idf::I2cBus *i2c_bus{};
#if !T_PANEL_P4_BSP_IS_RECT
        pdl::esp_idf::SpiBus *spi_bus{};
        pdl::expand_io::Xl9555 *xl9555{};
#endif
        // 芯片驱动与适配器；ES7210 采集通过 Board 音频 API 访问。
        pdl::charge::Axp517 *axp517{};
        TouchController *touch{};
        Es8389Device *es8389{};
        pdl::camera::Ov2710 *ov2710{};
        pdl::power::Sgm38121 *sgm38121{};
#if !T_PANEL_P4_BSP_IS_RECT
        LR2021 *lr2021{};
#endif
        // ESP-IDF 原生句柄：显示、音频和存储。
        esp_lcd_panel_handle_t display_panel{};
        esp_lcd_dsi_bus_handle_t dsi_bus{};
        esp_lcd_panel_io_handle_t display_io{};
        esp_codec_dev_handle_t audio_codec{};
        esp_codec_dev_handle_t audio_input_codec{};
        i2s_chan_handle_t i2s_tx{};
        i2s_chan_handle_t i2s_rx{};
        sdmmc_card_t *sd_card{};
    };

    // 统一硬件资源所有者；生命周期操作须由调用方串行执行。
    class Board
    {
    public:
        // ========================================================================
        // 3.1 板级常量与生命周期：src/board.cpp；构造/析构见 src/camera.cpp
        // ========================================================================
        static constexpr int displayWidth = 720;
#if !T_PANEL_P4_BSP_IS_RECT
        static constexpr int displayHeight = 720;
#else
        static constexpr int displayHeight = 1440;
#endif
        // Default panel depth; use displayConfig() for the active configuration.
        static constexpr std::uint8_t displayBitsPerPixel = 24;
        static constexpr std::uint8_t maximumFrameBufferCount = 3;
        static constexpr std::uint8_t maximumCameraBufferCount = 3;

        Board();
        ~Board();

        Board(const Board &) = delete;
        Board &operator=(const Board &) = delete;

        // Initializes every default peripheral. Existing devices keep their configuration.
        // On failure, newly created peripherals are rolled back; shared bus/power stay
        // available for retry until deinitialize(). Serialize lifecycle calls externally.
        [[nodiscard]] esp_err_t initialize();
        [[nodiscard]] esp_err_t deinitialize();
        [[nodiscard]] bool isInitialized() const noexcept;

        // Snapshot of individually ready devices, including after partial initialization.
        // Null means not ready. Pointers are borrowed and expire on device teardown.
        // Does not initialize devices or transfer ownership to the caller.
        [[nodiscard]] Devices devices() noexcept;

        // ========================================================================
        // 3.2 共享总线与 IO 扩展 API：src/board.cpp
        // ========================================================================
        // 创建/复用共享 I2C；各外设初始化时会自动准备所需总线。
        [[nodiscard]] esp_err_t initializeI2cBus();
        [[nodiscard]] bool isI2cBusInitialized() const noexcept;
#if !T_PANEL_P4_BSP_IS_RECT
        // 仅 Standard / Round：自动准备 I2C，并创建/复用 XL9555。
        [[nodiscard]] esp_err_t initializeIoExpander();
        [[nodiscard]] bool isIoExpanderInitialized() const noexcept;
#endif

        // ========================================================================
        // 3.3 电源、充电与电池 API：src/power.cpp；板级供电/休眠见 src/board.cpp
        // ========================================================================
        // Prepares I2C and applies PowerConfig defaults. Repeated calls reuse
        // the initialized controller; deinitialize first to apply a new config.
        [[nodiscard]] esp_err_t initializePower();
        [[nodiscard]] esp_err_t initializePower(const PowerConfig &config);
        [[nodiscard]] esp_err_t deinitializePower();
        [[nodiscard]] bool isPowerInitialized() const noexcept;
        [[nodiscard]] esp_err_t setPowerEventCallback(PowerEventCallback callback, void *context = nullptr);
        [[nodiscard]] esp_err_t readPowerStatus(pdl::charge::Axp517PowerStatus &status,
                                                std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readPowerModuleEnableStatus(
            pdl::charge::Axp517ModuleEnableStatus &status, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargerEnabled(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t getChargerEnabled(bool &enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedEnabled(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedOutputType(
            pdl::charge::Axp517ChargeLedOutputType type, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedMode(
            pdl::charge::Axp517ChargeLedMode mode, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setChargeLedCfgState(
            pdl::charge::Axp517ChargeLedCfgState state, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readChargeLedConfig(pdl::charge::Axp517ChargeLedConfig &config,
                                                    std::uint32_t timeout_ms = 100);
        // Controls both the AXP517 mask and GPIO delivery. Enabling uses the
        // initialization config's mask, even when interrupts started disabled.
        // May be called from the event callback; an in-flight callback can finish
        // after disabling. Serialize lifecycle operations externally.
        [[nodiscard]] esp_err_t setPowerInterruptEnabled(bool enabled);
        [[nodiscard]] bool isPowerInterruptEnabled() const noexcept;
        [[nodiscard]] esp_err_t readPowerFaults(pdl::charge::Axp517FaultStatus &status,
                                                std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t clearPowerFaults(std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readBatteryPercentage(std::uint8_t &percent,
                                                      std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readBatteryVoltageMv(std::uint16_t &millivolts,
                                                     std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readBatteryCurrentMa(std::int16_t &milliamps,
                                                     std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readVbusVoltageMv(std::uint16_t &millivolts,
                                                  std::uint32_t timeout_ms = 100);

        // Ship mode cuts the AXP517 battery BATFET and may remove power before
        // this function returns. External VBUS can keep the system powered.
        // Returns the first error and logs the operation that failed.
        [[nodiscard]] esp_err_t enterShipMode(std::uint32_t timeout_ms = 100);
        // Suspends the active display/touch resources as required by the board,
        // then restores them after wake. Errors identify the failed step in logs.
        [[nodiscard]] esp_err_t enterLightSleep();
        [[nodiscard]] esp_err_t enterLightSleep(const LightSleepConfig &config);
        // On success deep sleep does not return. With no wake source configured,
        // the board requires reset or a power cycle to boot again.
        [[nodiscard]] esp_err_t enterDeepSleep();
        [[nodiscard]] esp_err_t enterDeepSleep(const DeepSleepConfig &config);

        // ========================================================================
        // 3.4 显示与背光 API：src/display.cpp；setDisplaySleep 仅 Standard
        // ========================================================================
        [[nodiscard]] esp_err_t initializeDisplay();
        [[nodiscard]] esp_err_t initializeDisplay(const DisplayConfig &config);
        [[nodiscard]] esp_err_t deinitializeDisplay();
        [[nodiscard]] bool isDisplayInitialized() const noexcept;
        // Returns the active configuration, or defaults when uninitialized.
        [[nodiscard]] DisplayConfig displayConfig() const noexcept;

        [[nodiscard]] esp_err_t setBacklightBrightness(std::uint8_t percent);
        [[nodiscard]] std::uint8_t backlightBrightness() const noexcept;
#if T_PANEL_P4_BSP_IS_STANDARD
        // Light-sleep display control: suspend the DPI stream/GDMA around P4 light
        // sleep, resynchronize JD9365 with write-only commands, and toggle the
        // backlight. Resume performs no panel register reads.
        [[nodiscard]] esp_err_t setDisplaySleep(bool sleeping);
#endif

        // ========================================================================
        // 3.5 触摸与中断 API：src/touch.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializeTouch();
        [[nodiscard]] esp_err_t initializeTouch(const TouchConfig &config);
        [[nodiscard]] esp_err_t deinitializeTouch();
        [[nodiscard]] bool isTouchInitialized() const noexcept;
        [[nodiscard]] TouchConfig touchConfig() const noexcept;
        [[nodiscard]] esp_err_t initializeTouchInterrupt(
            pdl::esp_idf::GpioInterrupt::IsrHandler handler, void *argument = nullptr,
            gpio_int_type_t trigger = GPIO_INTR_NEGEDGE);
        [[nodiscard]] esp_err_t deinitializeTouchInterrupt();

        // ========================================================================
        // 3.6 摄像头控制、取帧与统计 API：src/camera.cpp
        // ========================================================================
        // Prepares the sensor and pipeline; call startCameraStreaming() to capture.
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
        [[nodiscard]] esp_err_t readCameraProductId(std::uint16_t &product_id,
                                                    std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t softwareResetCameraSensor(std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraTestPattern(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraHorizontalMirror(bool enabled,
                                                          std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraVerticalFlip(bool enabled, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t setCameraAutoExposureTarget(std::uint8_t target,
                                                            std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t readCameraSensorStatistics(
            pdl::camera::Ov2710Statistics &statistics, std::uint32_t timeout_ms = 100);
        [[nodiscard]] esp_err_t acquireCameraFrame(CameraFrame &frame,
                                                   std::uint32_t timeout_ms = 2000);
        [[nodiscard]] esp_err_t releaseCameraFrame(const std::uint8_t *frame);
        [[nodiscard]] pdl::camera::Ov2710ModeInfo cameraModeInfo() const noexcept;
        [[nodiscard]] CameraStatistics cameraStatistics() const noexcept;

        // ========================================================================
        // 3.7 音频录放、增益与统计 API：src/audio.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializeAudio();
        [[nodiscard]] esp_err_t initializeAudio(const AudioConfig &config);
        [[nodiscard]] esp_err_t reconfigureAudio(const AudioConfig &config);
        [[nodiscard]] esp_err_t deinitializeAudio();
        [[nodiscard]] bool isAudioInitialized() const noexcept;
        [[nodiscard]] AudioConfig audioConfig() const noexcept;
        [[nodiscard]] AudioStatistics audioStatistics() const noexcept;
        void resetAudioStatistics() noexcept;
        [[nodiscard]] esp_err_t readAudio(void *data, std::size_t size, std::size_t &bytes_read,
                                          std::uint32_t timeout_ms = 1000);
        [[nodiscard]] esp_err_t writeAudio(const void *data, std::size_t size,
                                           std::size_t &bytes_written,
                                           std::uint32_t timeout_ms = 1000);
        // Write enough silence to play out the queued DMA audio. The caller must
        // stop all other TX writers first. Does not change PA or codec mute state.
        [[nodiscard]] esp_err_t drainAudio(std::uint32_t timeout_ms = 1000);
        // Shared output volume used by music playback, recorded-audio playback
        // and the settings audio-volume panel. The value is in codec dB.
        [[nodiscard]] esp_err_t setAudioVolumeDb(float decibels);
        [[nodiscard]] float audioVolumeDb() const noexcept;
        [[nodiscard]] esp_err_t setAudioMuted(bool muted);
        [[nodiscard]] bool isAudioMuted() const noexcept;
        // Enabling output with PA off drains stale TX, enables PA and waits 10 ms.
        // Call before starting TX writers. For PA-only shutdown use setSpeakerEnabled.
        [[nodiscard]] esp_err_t setAudioOutputEnabled(bool enabled);
        [[nodiscard]] bool isAudioOutputEnabled() const noexcept;
        [[nodiscard]] esp_err_t setAudioCaptureEnabled(bool enabled);
        [[nodiscard]] bool isAudioCaptureEnabled() const noexcept;
        [[nodiscard]] esp_err_t setAudioCaptureMuted(bool muted);
        [[nodiscard]] bool isAudioCaptureMuted() const noexcept;
        [[nodiscard]] esp_err_t setMicrophoneGain(
            AudioMicrophoneGain gain,
            AudioMicrophoneChannel channels = AudioMicrophoneChannel::both);

        // ========================================================================
        // 3.8 WAV 录音 API：src/recording.cpp；依赖音频和存储
        // ========================================================================
        [[nodiscard]] esp_err_t startRecording(const char *path);
        [[nodiscard]] esp_err_t startRecording(const RecordingConfig &config);
        [[nodiscard]] esp_err_t stopRecording(std::uint32_t timeout_ms = 2000);
        [[nodiscard]] esp_err_t setRecordingEventCallback(RecordingEventCallback callback,
                                                          void *context = nullptr);
        [[nodiscard]] RecordingState recordingState() const noexcept;
        [[nodiscard]] esp_err_t recordingLastError() const noexcept;
        [[nodiscard]] std::uint32_t recordingBytes() const noexcept;
        [[nodiscard]] std::uint32_t recordingDurationMs() const noexcept;
        [[nodiscard]] bool isRecording() const noexcept;

        // ========================================================================
        // 3.9 音乐播放与元数据 API：src/music.cpp；依赖音频和存储
        // ========================================================================
        [[nodiscard]] esp_err_t playMusicFile(const char *path);
        [[nodiscard]] esp_err_t playMusicFile(const char *path, const MusicPlaybackConfig &config);
        [[nodiscard]] esp_err_t probeMusicFile(const char *path, MusicProbe &probe) const;
        [[nodiscard]] esp_err_t pauseMusic(bool paused);
        [[nodiscard]] esp_err_t stopMusic();
        [[nodiscard]] esp_err_t stopMusic(std::uint32_t timeout_ms);
        [[nodiscard]] esp_err_t deinitializeMusic();
        [[nodiscard]] esp_err_t setMusicEventCallback(MusicEventCallback callback,
                                                      void *context = nullptr);
        [[nodiscard]] esp_err_t setMusicNextTrackCallback(MusicNextTrackCallback callback,
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
        [[nodiscard]] esp_err_t fadeMusicVolumePercent(std::int32_t target_percent,
                                                       std::uint32_t duration_ms);
        [[nodiscard]] std::int32_t musicVolumePercent() const noexcept;
        [[nodiscard]] esp_err_t getMusicCover(std::vector<std::uint8_t> &cover) const;
        [[nodiscard]] esp_err_t getMusicLyrics(std::string &lyrics) const;
        [[nodiscard]] esp_err_t getMusicCover(const char *path,
                                              std::vector<std::uint8_t> &cover) const;
        [[nodiscard]] esp_err_t getMusicLyrics(const char *path, std::string &lyrics) const;
        [[nodiscard]] esp_err_t getMusicMetadata(const char *path, std::vector<std::uint8_t> &cover,
                                                 std::string &lyrics) const;

        // ========================================================================
        // 3.10 LoRa API：仅 Standard / Round；src/lora.cpp
        // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] esp_err_t initializeLora();
        [[nodiscard]] esp_err_t initializeLora(const LoraConfig &config);
        [[nodiscard]] esp_err_t deinitializeLora();
        [[nodiscard]] bool isLoraInitialized() const noexcept;
        [[nodiscard]] esp_err_t loraStandby();
        [[nodiscard]] esp_err_t loraSleep();
        [[nodiscard]] std::int16_t loraLastError() const noexcept;
#endif

        // ========================================================================
        // 3.11 SD 卡与 USB MSC API：src/sd_card.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializeSdCard();
        [[nodiscard]] esp_err_t initializeSdCard(const SdCardConfig &config);
        [[nodiscard]] esp_err_t deinitializeSdCard();
        [[nodiscard]] bool isSdCardInitialized() const noexcept;
        [[nodiscard]] bool isSdCardMounted() const noexcept;
        // USB MSC 使用原始 SD 块设备；与本地文件系统访问互斥。
        [[nodiscard]] esp_err_t initializeUsbMsc();
        [[nodiscard]] esp_err_t deinitializeUsbMsc();
        [[nodiscard]] bool isUsbMscInitialized() const noexcept;

        // ========================================================================
        // 3.12 板级使能与复位 API：src/board.cpp；默认控制路径随板型选择
        // ========================================================================
        // true selects XL9555, false selects P4 GPIO, using the board's pins value.
        // Defaults follow the board wiring. Rect does not have an XL9555.
        [[nodiscard]] esp_err_t setSpeakerEnabled(bool enabled, bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);
        // Reads the current pin level; returns false if unavailable or the read fails.
        [[nodiscard]] bool isSpeakerEnabled(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT) const noexcept;
        [[nodiscard]] esp_err_t setSdPowerEnabled(bool enabled, bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);
        [[nodiscard]] esp_err_t setLoraPowerEnabled(bool enabled, bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);
        [[nodiscard]] esp_err_t setC5AudioLcdEnabled(bool enabled, bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);
        // Reads the current pin level; returns false if unavailable or the read fails.
        [[nodiscard]] bool isC5AudioLcdEnabled(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT) const noexcept;
#if T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] bool isDisplayPowerEnabled(bool isIOExpander = false) const noexcept;
        [[nodiscard]] esp_err_t setDisplayPowerEnabled(bool enabled, bool isIOExpander = false);
#endif
        [[nodiscard]] esp_err_t resetTouchController(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);

        // ========================================================================
        // 3.13 底层设备、原生句柄与帧缓冲访问 API：各外设实现文件
        // ========================================================================
        // 设备指针/句柄由 Board 持有；空值约定见各接口。
        // 释放或重建相应资源后，须重新获取借用的设备指针/句柄。
        [[nodiscard]] pdl::esp_idf::I2cBus *i2cBus() noexcept;
#if !T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] pdl::esp_idf::SpiBus *spiBus() noexcept;
        [[nodiscard]] pdl::expand_io::Xl9555 *ioExpander() noexcept;
#endif
        [[nodiscard]] TouchController *touchController() noexcept;
        // Output handle; also owns ES8389 capture when no separate ADC is fitted.
        [[nodiscard]] esp_codec_dev_handle_t audioCodec() noexcept;
        [[nodiscard]] esp_codec_dev_handle_t audioInputCodec() noexcept;
        [[nodiscard]] i2s_chan_handle_t audioTransmitChannel() const noexcept;
        [[nodiscard]] i2s_chan_handle_t audioReceiveChannel() const noexcept;
#if !T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] LR2021 *loraRadio() noexcept;
        [[nodiscard]] const LoraConfig *loraConfig() const noexcept;
#endif
        [[nodiscard]] sdmmc_card_t *sdCard() noexcept;
        [[nodiscard]] const char *sdCardMountPoint() const noexcept;
        // Common ESP-IDF panel handle for all profiles. Borrowed; null until initialized.
        [[nodiscard]] esp_lcd_panel_handle_t displayPanel() const noexcept;
        [[nodiscard]] void *frameBuffer(std::uint8_t index = 0) const noexcept;
        [[nodiscard]] std::uint8_t frameBufferCount() const noexcept;

    private:
        friend class Es8389Device;

        // ========================================================================
        // 4.1 共享总线与板级控制：src/board.cpp
        // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
        [[nodiscard]] esp_err_t setExpanderOutput(std::uint8_t pin, bool high);
#endif
        [[nodiscard]] esp_err_t setControlOutput(int pin, bool high, bool isIOExpander);
        [[nodiscard]] bool controlPinHigh(int pin, bool isIOExpander) const noexcept;
        [[nodiscard]] esp_err_t prepareForPowerDown();

        // 总线先于设备声明；同组内 transport 先于驱动，保证逆序析构时依赖仍有效。
        pdl::esp_idf::I2cBus i2c_bus_;
#if !T_PANEL_P4_BSP_IS_RECT
        pdl::esp_idf::SpiBus spi_bus_;
        std::optional<pdl::esp_idf::I2cDevice> xl9555_transport_;
        // Live pin reads update the driver's input history, including in const queries.
        mutable std::optional<pdl::expand_io::Xl9555> xl9555_;
#endif

        // ========================================================================
        // 4.2 AXP517、供电事件与中断任务：src/power.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializePowerController(const PowerConfig &config);
        static void powerInterruptHandler(void *argument);
        static void powerEventTaskEntry(void *argument);
        void stopPowerEventTask() noexcept;

        std::optional<pdl::esp_idf::I2cDevice> axp517_transport_;
        std::optional<pdl::charge::Axp517> axp517_;
        pdl::esp_idf::GpioInterrupt power_interrupt_;
        TaskHandle_t power_event_task_{};
        SemaphoreHandle_t power_event_stopped_{};
        SemaphoreHandle_t power_interrupt_mutex_{};
        std::atomic<bool> power_interrupt_enabled_{false};
        std::atomic<bool> power_task_stop_{false};
        std::atomic<PowerEventCallback> power_event_callback_{nullptr};
        std::atomic<void *> power_event_context_{nullptr};
        PowerConfig power_config_{};

        // ========================================================================
        // 4.3 MIPI DSI / DPI、面板与背光：src/display.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t initializeBacklight();
        [[nodiscard]] esp_err_t deinitializeBacklight();
        [[nodiscard]] esp_err_t assertDisplayReset(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);
        [[nodiscard]] esp_err_t releaseDisplayReset(bool isIOExpander = !T_PANEL_P4_BSP_IS_RECT);

        esp_ldo_channel_handle_t mipi_phy_ldo_{};
        esp_lcd_dsi_bus_handle_t dsi_bus_{};
        esp_lcd_panel_io_handle_t dbi_io_{};
        esp_lcd_panel_handle_t panel_{};
        std::array<void *, maximumFrameBufferCount> frame_buffers_{};

        std::uint8_t frame_buffer_count_{};
        DisplayConfig display_config_{};
        std::uint8_t backlight_brightness_{};
        bool backlight_initialized_{};
        bool panel_initialized_{};
        bool display_initialized_{};
#if T_PANEL_P4_BSP_IS_STANDARD
        bool display_sleeping_{};
        std::uint8_t display_sleep_brightness_{};
#endif

        // ========================================================================
        // 4.4 触摸驱动与 ISR 转发：src/touch.cpp；复位时序见 src/board.cpp
        // ========================================================================
        [[nodiscard]] esp_err_t resetTouchControllerImpl(bool isIOExpander,
                                                        bool restore_interrupt_input);
        std::optional<pdl::esp_idf::I2cDevice> touch_transport_;
        std::optional<TouchController> touch_;
        TouchConfig touch_config_{};
        static void touchInterruptHandler(void *argument);
        std::atomic<pdl::esp_idf::GpioInterrupt::IsrHandler> touch_interrupt_handler_{nullptr};
        std::atomic<void *> touch_interrupt_argument_{nullptr};
        pdl::esp_idf::GpioInterrupt touch_interrupt_;

        // ========================================================================
        // 4.5 OV2710 / CSI / ISP：src/camera.cpp；CameraState 隐藏管线实现
        // ========================================================================
        void populateCameraDevices(Devices &devices) noexcept;
        pdl::esp_idf::TaskDelay task_delay_;
        std::unique_ptr<CameraState> camera_state_;

        // ========================================================================
        // 4.6 ES8389 / ES7210、I2S、音频开关与统计：src/audio.cpp
        // ========================================================================
        Es8389Device es8389_{*this};
        const audio_codec_data_if_t *audio_data_if_{};
        const audio_codec_ctrl_if_t *audio_ctrl_if_{};
        const audio_codec_if_t *audio_codec_if_{};
        esp_codec_dev_handle_t audio_codec_{};
        const audio_codec_ctrl_if_t *audio_input_ctrl_if_{};
        const audio_codec_if_t *audio_input_codec_if_{};
        esp_codec_dev_handle_t audio_input_codec_{};
        i2s_chan_handle_t audio_tx_{};
        i2s_chan_handle_t audio_rx_{};

        bool audio_initialized_{};
        bool audio_tx_enabled_{};
        bool audio_rx_enabled_{};
        bool audio_capture_enabled_{};
        bool audio_capture_muted_{};
        bool audio_output_enabled_{};
        bool audio_output_muted_{};
        AudioConfig audio_config_{};
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

        // ========================================================================
        // 4.7 录音任务、PSRAM 缓冲与 WAV 文件：src/recording.cpp
        // ========================================================================
        static void recordingCaptureTaskEntry(void *argument);
        static void recordingWriterTaskEntry(void *argument);
        void notifyRecordingEvent(RecordingEventType type, RecordingState state,
                                  esp_err_t error = ESP_OK, const std::uint8_t *data = nullptr,
                                  std::size_t data_size = 0U);

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
        std::uint8_t *recording_capture_buffer_{};
        std::uint8_t *recording_writer_buffer_{};
        FILE *recording_file_{};
        std::array<char, 192> recording_path_{};
        std::size_t recording_chunk_bytes_{};
        std::uint32_t recording_read_timeout_ms_{};
        std::uint32_t recording_sample_rate_{};
        std::uint16_t recording_channels_{};
        std::uint16_t recording_bits_per_sample_{};

        // ========================================================================
        // 4.8 音乐运行时：src/music.cpp；MusicRuntime 隐藏播放器实现
        // ========================================================================
        [[nodiscard]] MusicRuntime *ensureMusicRuntime();
        MusicRuntime *music_runtime_{};

        // ========================================================================
        // 4.9 LR2021 / RadioLib：仅 Standard / Round；src/lora.cpp
        // ========================================================================
#if !T_PANEL_P4_BSP_IS_RECT
        std::unique_ptr<RadioLibHal> lora_hal_;
        std::unique_ptr<Module> lora_module_;
        std::unique_ptr<LR2021> lora_radio_;
        std::optional<LoraConfig> lora_config_;
        bool lora_initialized_{};
        std::int16_t lora_last_error_{};
#endif

        // ========================================================================
        // 4.10 SDMMC / FAT 与 USB MSC 资源：src/sd_card.cpp
        // ========================================================================
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
    };
}

// 当前构建板型的常用入口；完整配置类型位于 T_PANEL_P4_BSP_NAMESPACE。
namespace bsp
{
    using Board = T_PANEL_P4_BSP_NAMESPACE::Board;
    using Devices = T_PANEL_P4_BSP_NAMESPACE::Devices;
    using DisplayConfig = T_PANEL_P4_BSP_NAMESPACE::DisplayConfig;
    using TouchConfig = T_PANEL_P4_BSP_NAMESPACE::TouchConfig;
}
