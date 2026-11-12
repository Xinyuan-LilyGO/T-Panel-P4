#include <bsp/board.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <es7210_adc.h>
#include <es8389_codec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{
    constexpr char kTag[] = "t_panel_p4_audio";
    constexpr float kEs8389MinVolumeDb = -95.5F;
    constexpr float kEs8389MaxVolumeDb = 32.0F;

    // One output-volume value is shared by music playback, recorded-audio
    // playback and the settings panel.  Keep it outside Board so a temporary
    // audio teardown does not create a second volume state.
    std::atomic<float> s_public_audio_volume_db{0.0F};

    void preserveFirstError(esp_err_t candidate, esp_err_t &first) noexcept
    {
        if (first == ESP_OK && candidate != ESP_OK)
            first = candidate;
    }

    esp_err_t updateVerifiedBits(const audio_codec_ctrl_if_t *ctrl, int reg, std::uint8_t mask, std::uint8_t bits)
    {
        if (ctrl == nullptr)
            return ESP_ERR_INVALID_STATE;
        std::uint8_t value{};
        if (ctrl->read_reg(ctrl, reg, 1, &value, 1) != ESP_CODEC_DEV_OK)
            return ESP_FAIL;
        value = (value & ~mask) | (bits & mask);
        if (ctrl->write_reg(ctrl, reg, 1, &value, 1) != ESP_CODEC_DEV_OK)
            return ESP_FAIL;
        std::uint8_t actual{};
        if (ctrl->read_reg(ctrl, reg, 1, &actual, 1) != ESP_CODEC_DEV_OK)
            return ESP_FAIL;
        if ((actual & mask) != (value & mask))
        {
            ESP_LOGE(kTag, "register 0x%02x verification failed: requested=0x%02x actual=0x%02x",
                     reg, value, actual);
            return ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_OK;
    }

    esp_err_t setDacMuted(const audio_codec_ctrl_if_t *ctrl, bool muted)
    {
        // R40[1:0] mutes DAC inputs. The bundled output-mute callback uses R20
        // (ADC outputs), so keep playback and capture mute independent here.
        return updateVerifiedBits(ctrl, 0x40, 0x03, muted ? 0x03 : 0x00);
    }

    esp_err_t setCaptureMuted(const audio_codec_ctrl_if_t *ctrl, bool es7210, bool muted)
    {
        if (!es7210)
            return updateVerifiedBits(ctrl, 0x20, 0x03, muted ? 0x03 : 0x00);
        esp_err_t error = updateVerifiedBits(ctrl, 0x14, 0x03, muted ? 0x03 : 0x00);
        if (error == ESP_OK)
            error = updateVerifiedBits(ctrl, 0x15, 0x03, muted ? 0x03 : 0x00);
        return error;
    }

    esp_err_t configureOutputOnly(const audio_codec_ctrl_if_t *ctrl)
    {
        // The vendor ES8389 driver initializes both paths even in DAC mode.
        // R01[5] releases ASDOUT so a separate ADC can drive the input bus.
        // R64 disables ADC reference/PGA/modulators and holds both ADCs in reset.
        esp_err_t error = setCaptureMuted(ctrl, false, true);
        if (error == ESP_OK)
            error = updateVerifiedBits(ctrl, 0x01, 0x20, 0x20);
        if (error == ESP_OK)
            error = updateVerifiedBits(ctrl, 0x64, 0xbf, 0x30);
        return error;
    }

    TickType_t timeoutTicks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == 0U)
            return 0;
        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        return ticks == 0 ? 1 : ticks;
    }

    // Keep this adapter in the BSP: the default codec adapter drops the failing
    // register and esp_err_t, making a DVDD/reset failure look like an address error.
    struct CodecControl
    {
        audio_codec_ctrl_if_t base;
        i2c_master_dev_handle_t device;
        const char *name;
        bool bootstrapping;
        esp_err_t last_error;
    };

    bool codecControlIsOpen(const audio_codec_ctrl_if_t *ctrl)
    {
        return ctrl != nullptr && reinterpret_cast<const CodecControl *>(ctrl)->device != nullptr;
    }

    int codecReadRegister(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len)
    {
        if (!codecControlIsOpen(ctrl) || reg_len != 1 || reg < 0 || reg > 0xff || data == nullptr ||
            data_len != 1)
            return ESP_CODEC_DEV_INVALID_ARG;
        auto *control = reinterpret_cast<CodecControl *>(const_cast<audio_codec_ctrl_if_t *>(ctrl));
        const std::uint8_t address = static_cast<std::uint8_t>(reg);
        const esp_err_t error = i2c_master_transmit_receive(
            control->device, &address, 1, static_cast<std::uint8_t *>(data), 1, 100);
        if (error != ESP_OK)
        {
            control->last_error = error;
            ESP_LOGE(kTag, "%s read reg 0x%02x failed: %s", control->name, reg,
                     esp_err_to_name(error));
        }
        return error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
    }

    int codecWriteRegister(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len)
    {
        if (!codecControlIsOpen(ctrl) || reg_len != 1 || reg < 0 || reg > 0xff || data == nullptr ||
            data_len != 1)
            return ESP_CODEC_DEV_INVALID_ARG;
        auto *control = reinterpret_cast<CodecControl *>(const_cast<audio_codec_ctrl_if_t *>(ctrl));
        const std::uint8_t bytes[]{static_cast<std::uint8_t>(reg),
                                   *static_cast<const std::uint8_t *>(data)};
        const esp_err_t error = i2c_master_transmit(control->device, bytes, sizeof(bytes), 100);
        if (error != ESP_OK)
        {
            control->last_error = error;
            ESP_LOGE(kTag, "%s write reg 0x%02x=0x%02x failed: %s", control->name, reg, bytes[1],
                     esp_err_to_name(error));
        }
        // Isolation/reset writes change which domain serves subsequent I2C
        // transactions. Allow it to settle during construction, with clocks on.
        if (error == ESP_OK && control->bootstrapping &&
            (reg == 0xf3 || (reg == 0x00 && bytes[1] == 0x7e)))
            vTaskDelay(timeoutTicks(10));
        return error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
    }

    int codecControlClose(const audio_codec_ctrl_if_t *ctrl)
    {
        auto *control = reinterpret_cast<CodecControl *>(const_cast<audio_codec_ctrl_if_t *>(ctrl));
        if (control == nullptr || control->device == nullptr)
            return ESP_CODEC_DEV_OK;
        const esp_err_t error = i2c_master_bus_rm_device(control->device);
        if (error != ESP_OK)
            ESP_LOGE(kTag, "remove %s I2C device failed: %s", control->name,
                     esp_err_to_name(error));
        else
            control->device = nullptr;
        return error == ESP_OK ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR;
    }

    esp_err_t createCodecControl(i2c_master_bus_handle_t bus, std::uint8_t address, const char *name, bool bootstrapping, const audio_codec_ctrl_if_t **result)
    {
        auto *control = static_cast<CodecControl *>(std::calloc(1, sizeof(CodecControl)));
        if (control == nullptr)
            return ESP_ERR_NO_MEM;
        control->base.is_open = codecControlIsOpen;
        control->base.read_reg = codecReadRegister;
        control->base.write_reg = codecWriteRegister;
        control->base.close = codecControlClose;
        control->name = name;
        control->bootstrapping = bootstrapping;
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 100000;
        const esp_err_t error = i2c_master_bus_add_device(bus, &config, &control->device);
        if (error != ESP_OK)
        {
            std::free(control);
            return error;
        }
        *result = &control->base;
        return ESP_OK;
    }

    esp_err_t probeCodec(i2c_master_bus_handle_t bus, std::uint8_t address, const char *name)
    {
        esp_err_t error = ESP_ERR_NOT_FOUND;
        for (unsigned attempt = 0; attempt < 5U; ++attempt)
        {
            error = i2c_master_probe(bus, address, 100);
            if (error == ESP_OK)
                return ESP_OK;
            if (attempt < 4U)
                vTaskDelay(timeoutTicks(20));
        }
        ESP_LOGE(kTag, "%s not responding at I2C 7-bit address 0x%02x: %s", name, address,
                 esp_err_to_name(error));
        return error;
    }

    esp_err_t releaseChannel(i2s_chan_handle_t &channel)
    {
        if (channel == nullptr)
            return ESP_OK;
        i2s_chan_info_t info{};
        esp_err_t error = i2s_channel_get_info(channel, &info);
        if (error == ESP_OK && info.is_enabled)
            error = i2s_channel_disable(channel);
        if (error == ESP_OK)
            error = i2s_del_channel(channel);
        if (error == ESP_OK)
            channel = nullptr;
        return error;
    }

    esp_err_t prepareCodecRegisters(const audio_codec_ctrl_if_t *ctrl)
    {
        // FD/FE live in PVDD and remain readable while the DVDD register bank is
        // isolated. Test the reset register as well, before the vendor's sequence.
        for (unsigned attempt = 0; attempt < 5U; ++attempt)
        {
            // F3 bit 0 resets the DVDD logic even when a host reset did not
            // discharge the peripheral rail. Release it before accessing R00.
            std::uint8_t isolation_reset = 0x01;
            std::uint8_t isolation = 0x00;
            std::uint8_t reset = 0x7e;
            std::uint8_t actual{};
            if (ctrl->write_reg(ctrl, 0xf3, 1, &isolation_reset, 1) == ESP_CODEC_DEV_OK &&
                ctrl->write_reg(ctrl, 0xf3, 1, &isolation, 1) == ESP_CODEC_DEV_OK &&
                ctrl->write_reg(ctrl, 0x00, 1, &reset, 1) == ESP_CODEC_DEV_OK &&
                ctrl->read_reg(ctrl, 0x00, 1, &actual, 1) == ESP_CODEC_DEV_OK && actual == reset)
            {
                ESP_LOGI(kTag, "ES8389 DVDD register bank ready (reset=0x%02x)", actual);
                return ESP_OK;
            }
            ESP_LOGW(kTag, "ES8389 DVDD not ready, attempt %u/5, reset readback=0x%02x",
                     attempt + 1U, actual);
            vTaskDelay(timeoutTicks(20));
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool validMclk(i2s_mclk_multiple_t value) noexcept
    {
        switch (value)
        {
        case I2S_MCLK_MULTIPLE_128:
        case I2S_MCLK_MULTIPLE_192:
        case I2S_MCLK_MULTIPLE_256:
        case I2S_MCLK_MULTIPLE_384:
        case I2S_MCLK_MULTIPLE_512:
        case I2S_MCLK_MULTIPLE_576:
        case I2S_MCLK_MULTIPLE_768:
        case I2S_MCLK_MULTIPLE_1024:
        case I2S_MCLK_MULTIPLE_1152:
            return true;
        default:
            return false;
        }
    }

    bool toI2sWidth(bsp::T_PANEL_P4_BSP_NAMESPACE::AudioWordLength value, i2s_data_bit_width_t &width) noexcept
    {
        using bsp::T_PANEL_P4_BSP_NAMESPACE::AudioWordLength;
        switch (value)
        {
        case AudioWordLength::bits16:
            width = I2S_DATA_BIT_WIDTH_16BIT;
            return true;
        case AudioWordLength::bits24:
            width = I2S_DATA_BIT_WIDTH_24BIT;
            return true;
        case AudioWordLength::bits32:
            width = I2S_DATA_BIT_WIDTH_32BIT;
            return true;
        default:
            return false;
        }
    }

    bool validSlots(i2s_slot_mode_t mode, i2s_std_slot_mask_t mask) noexcept
    {
        return (mode == I2S_SLOT_MODE_MONO || mode == I2S_SLOT_MODE_STEREO) && mask != 0 &&
               !(mode == I2S_SLOT_MODE_MONO && mask == I2S_STD_SLOT_BOTH);
    }

    std::uint8_t volumePercent(float db) noexcept
    {
        const float normalized =
            (db - kEs8389MinVolumeDb) / (kEs8389MaxVolumeDb - kEs8389MinVolumeDb);
        return static_cast<std::uint8_t>(std::clamp(normalized * 100.0F, 0.0F, 100.0F) + 0.5F);
    }

    float gainDb(bsp::T_PANEL_P4_BSP_NAMESPACE::AudioMicrophoneGain gain) noexcept
    {
        using G = bsp::T_PANEL_P4_BSP_NAMESPACE::AudioMicrophoneGain;
        switch (gain)
        {
        case G::db0:
            return 0.0F;
        case G::db3_5:
            return 3.5F;
        case G::db6_5:
            return 6.5F;
        case G::db9_5:
            return 9.5F;
        case G::db12_5:
            return 12.5F;
        case G::db15_5:
            return 15.5F;
        case G::db18_5:
            return 18.5F;
        case G::db21_5:
            return 21.5F;
        case G::db24_5:
            return 24.5F;
        case G::db27_5:
            return 27.5F;
        case G::db30_5:
            return 30.5F;
        case G::db33_5:
            return 33.5F;
        case G::db36_5:
            return 36.5F;
        default:
            return std::numeric_limits<float>::quiet_NaN();
        }
    }

    bool sameConfig(const bsp::T_PANEL_P4_BSP_NAMESPACE::AudioConfig &a, const bsp::T_PANEL_P4_BSP_NAMESPACE::AudioConfig &b) noexcept
    {
        return a.codec == b.codec && a.sample_rate_hz == b.sample_rate_hz &&
               a.word_length == b.word_length && a.mclk_multiple == b.mclk_multiple &&
               a.slot_mode == b.slot_mode && a.slot_mask == b.slot_mask &&
               a.dma_descriptor_count == b.dma_descriptor_count &&
               a.dma_frames_per_descriptor == b.dma_frames_per_descriptor &&
               a.output_volume_db == b.output_volume_db && a.microphone_gain == b.microphone_gain &&
               a.no_dac_reference == b.no_dac_reference && a.enable_capture == b.enable_capture &&
               a.enable_speaker == b.enable_speaker;
    }

    esp_err_t validateConfig(const bsp::T_PANEL_P4_BSP_NAMESPACE::AudioConfig &config, i2s_data_bit_width_t &width) noexcept
    {
        if (config.codec != bsp::T_PANEL_P4_BSP_NAMESPACE::AudioCodecChip::es8389 ||
            config.sample_rate_hz == 0U || config.dma_descriptor_count <= 0 ||
            config.dma_frames_per_descriptor <= 0 || !std::isfinite(config.output_volume_db) ||
            !std::isfinite(gainDb(config.microphone_gain)) ||
            config.output_volume_db < kEs8389MinVolumeDb ||
            config.output_volume_db > kEs8389MaxVolumeDb || !validMclk(config.mclk_multiple) ||
            !validSlots(config.slot_mode, config.slot_mask) ||
            !toI2sWidth(config.word_length, width))
            return ESP_ERR_INVALID_ARG;
        return ESP_OK;
    }
} // namespace

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    bool Es8389Device::isInitialized() const noexcept
    {
        return board_.isAudioInitialized();
    }

    esp_err_t Es8389Device::readChipId(std::uint16_t &chip_id, std::uint32_t timeout_ms) const
    {
        std::uint8_t high{};
        std::uint8_t low{};
        esp_err_t error = readRegister(0xfd, high, timeout_ms);
        if (error == ESP_OK)
            error = readRegister(0xfe, low, timeout_ms);
        if (error == ESP_OK)
            chip_id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) | low);
        return error;
    }

    esp_err_t Es8389Device::readRegister(std::uint8_t address, std::uint8_t &value, std::uint32_t timeout_ms) const
    {
        if (!isInitialized() || !codecControlIsOpen(board_.audio_ctrl_if_))
            return ESP_ERR_INVALID_STATE;
        if (timeout_ms == 0U ||
            timeout_ms > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            return ESP_ERR_INVALID_ARG;
        const auto *control = reinterpret_cast<const CodecControl *>(board_.audio_ctrl_if_);
        std::uint8_t result{};
        const esp_err_t error = i2c_master_transmit_receive(control->device, &address, 1, &result,
                                                            1, static_cast<int>(timeout_ms));
        if (error == ESP_OK)
            value = result;
        return error;
    }

    esp_err_t Es8389Device::writeRegister(std::uint8_t address, std::uint8_t value, std::uint32_t timeout_ms)
    {
        if (!isInitialized() || !codecControlIsOpen(board_.audio_ctrl_if_))
            return ESP_ERR_INVALID_STATE;
        if (timeout_ms == 0U ||
            timeout_ms > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            return ESP_ERR_INVALID_ARG;
        const auto *control = reinterpret_cast<const CodecControl *>(board_.audio_ctrl_if_);
        const std::uint8_t bytes[]{address, value};
        return i2c_master_transmit(control->device, bytes, sizeof(bytes),
                                   static_cast<int>(timeout_ms));
    }

    esp_err_t Es8389Device::setVolumeDb(float decibels)
    {
        return board_.setAudioVolumeDb(decibels);
    }

    float Es8389Device::volumeDb() const noexcept
    {
        return board_.audioVolumeDb();
    }

    esp_err_t Es8389Device::setMuted(bool muted)
    {
        return board_.setAudioMuted(muted);
    }

    bool Es8389Device::isMuted() const noexcept
    {
        return board_.isAudioMuted();
    }

    esp_err_t Es8389Device::setMicrophoneGain(AudioMicrophoneGain gain, AudioMicrophoneChannel channels)
    {
        if (pins::audioCaptureUsesEs7210)
            return ESP_ERR_NOT_SUPPORTED;
        return board_.setMicrophoneGain(gain, channels);
    }

    esp_err_t Board::initializeAudio()
    {
        return initializeAudio(AudioConfig{});
    }

    esp_err_t Board::initializeAudio(const AudioConfig &config)
    {
        if (isAudioInitialized())
            return ESP_OK;

        i2s_data_bit_width_t width{};
        esp_err_t error = validateConfig(config, width);
        if (error != ESP_OK)
            return error;

        ESP_RETURN_ON_ERROR(initializePower(), kTag, "initialize device power failed");

        // A prior partial cleanup must not leak channels into a new attempt.
        ESP_RETURN_ON_ERROR(deinitializeAudio(), kTag, "clean up previous audio state failed");
        const auto fail = [this](esp_err_t cause)
        {
            const esp_err_t cleanup = deinitializeAudio();
            if (cleanup != ESP_OK)
                ESP_LOGE(kTag, "audio initialization cleanup failed: %s", esp_err_to_name(cleanup));
            return cause;
        };

        error = setSpeakerEnabled(false);
        if (error != ESP_OK)
            return error;

        const bool es8389_capture = config.enable_capture && !pins::audioCaptureUsesEs7210;
        error = probeCodec(i2c_bus_.nativeHandle(), ES8389_CODEC_DEFAULT_ADDR >> 1, "ES8389");
        if (error == ESP_OK && config.enable_capture && pins::audioCaptureUsesEs7210)
            error = probeCodec(i2c_bus_.nativeHandle(), ES7210_CODEC_DEFAULT_ADDR >> 1, "ES7210");
        if (error != ESP_OK)
            return fail(error);

        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        chan_cfg.dma_desc_num = config.dma_descriptor_count;
        chan_cfg.dma_frame_num = config.dma_frames_per_descriptor;
        error =
            i2s_new_channel(&chan_cfg, &audio_tx_, config.enable_capture ? &audio_rx_ : nullptr);
        if (error != ESP_OK)
            return fail(error);

        i2s_std_config_t i2s_cfg{};
        i2s_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config.sample_rate_hz);
        i2s_cfg.clk_cfg.mclk_multiple = config.mclk_multiple;
        i2s_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(width, config.slot_mode);
        i2s_cfg.slot_cfg.slot_mask = config.slot_mask;
        i2s_cfg.gpio_cfg.mclk = pins::i2sMasterClock;
        i2s_cfg.gpio_cfg.bclk = pins::i2sBitClock;
        i2s_cfg.gpio_cfg.ws = pins::i2sWordSelect;
        i2s_cfg.gpio_cfg.dout = pins::i2sDataOut;
        i2s_cfg.gpio_cfg.din = pins::i2sDataIn;
        error = i2s_channel_init_std_mode(audio_tx_, &i2s_cfg);
        if (error == ESP_OK && audio_rx_ != nullptr)
            error = i2s_channel_init_std_mode(audio_rx_, &i2s_cfg);
        if (error != ESP_OK)
            return fail(error);

        audio_codec_i2s_cfg_t data_cfg{};
        data_cfg.port = I2S_NUM_0;
        data_cfg.tx_handle = audio_tx_;
        data_cfg.rx_handle = audio_rx_;
        data_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        audio_data_if_ = audio_codec_new_i2s_data(&data_cfg);
        if (audio_data_if_ == nullptr)
            return fail(ESP_ERR_NO_MEM);

        error = createCodecControl(i2c_bus_.nativeHandle(), ES8389_CODEC_DEFAULT_ADDR >> 1,
                                   "ES8389", true, &audio_ctrl_if_);
        if (error != ESP_OK)
            return fail(error);

        // es8389_codec_new() writes registers immediately, before
        // esp_codec_dev_open() normally starts I2S. Supply MCLK/BCLK/LRCK during
        // that sequence too, including when a warm codec retains its clock setup.
        error = i2s_channel_enable(audio_tx_);
        if (error != ESP_OK)
            return fail(error);
        audio_tx_enabled_ = true;
        vTaskDelay(timeoutTicks(10));
        ESP_LOGI(kTag, "I2S TX started before ES8389 register initialization: MCLK GPIO%d, %lu Hz",
                 static_cast<int>(pins::i2sMasterClock),
                 static_cast<unsigned long>(config.sample_rate_hz) *
                     static_cast<unsigned long>(config.mclk_multiple));

        std::uint8_t chip_id_high{};
        std::uint8_t chip_id_low{};
        int codec_result = audio_ctrl_if_->read_reg(audio_ctrl_if_, 0xfd, 1, &chip_id_high, 1);
        if (codec_result == ESP_CODEC_DEV_OK)
            codec_result = audio_ctrl_if_->read_reg(audio_ctrl_if_, 0xfe, 1, &chip_id_low, 1);
        if (codec_result != ESP_CODEC_DEV_OK)
        {
            ESP_LOGE(kTag, "ES8389 chip-ID read failed with I2S clocks running: codec error %d",
                     codec_result);
            return fail(ESP_FAIL);
        }
        ESP_LOGI(kTag, "ES8389 chip-ID registers: FD=0x%02x FE=0x%02x", chip_id_high, chip_id_low);
        if (chip_id_high != 0x83 || chip_id_low != 0x89)
            return fail(ESP_ERR_INVALID_RESPONSE);
        error = prepareCodecRegisters(audio_ctrl_if_);
        if (error != ESP_OK)
            return fail(error);

        es8389_codec_cfg_t es_cfg{};
        es_cfg.ctrl_if = audio_ctrl_if_;
        es_cfg.codec_mode =
            es8389_capture ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC;
        es_cfg.pa_pin = -1;
        es_cfg.master_mode = false;
        es_cfg.use_mclk = true;
        es_cfg.no_dac_ref = !es8389_capture || config.no_dac_reference;
        es_cfg.mclk_div = static_cast<std::uint16_t>(config.mclk_multiple);
        auto *output_control =
            reinterpret_cast<CodecControl *>(const_cast<audio_codec_ctrl_if_t *>(audio_ctrl_if_));
        output_control->last_error = ESP_OK;
        audio_codec_if_ = es8389_codec_new(&es_cfg);
        reinterpret_cast<CodecControl *>(const_cast<audio_codec_ctrl_if_t *>(audio_ctrl_if_))
            ->bootstrapping = false;
        if (audio_codec_if_ == nullptr || output_control->last_error != ESP_OK)
        {
            ESP_LOGE(kTag, "ES8389 register initialization failed with I2S clocks running");
            return fail(output_control->last_error != ESP_OK ? output_control->last_error
                                                             : ESP_FAIL);
        }
        if (!es8389_capture)
        {
            error = configureOutputOnly(audio_ctrl_if_);
            if (error != ESP_OK)
                return fail(error);
        }

        // A separate ADC is optional hardware, selected by topology rather than board model.
        if (config.enable_capture && pins::audioCaptureUsesEs7210)
        {
            error = createCodecControl(i2c_bus_.nativeHandle(), ES7210_CODEC_DEFAULT_ADDR >> 1,
                                       "ES7210", false, &audio_input_ctrl_if_);
            if (error != ESP_OK)
                return fail(error);
            es7210_codec_cfg_t adc_cfg{};
            adc_cfg.ctrl_if = audio_input_ctrl_if_;
            adc_cfg.master_mode = false;
            adc_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2;
            adc_cfg.mclk_src = ES7210_MCLK_FROM_PAD;
            adc_cfg.mclk_div = static_cast<std::uint16_t>(config.mclk_multiple);
            audio_input_codec_if_ = es7210_codec_new(&adc_cfg);
            const auto *input_control =
                reinterpret_cast<const CodecControl *>(audio_input_ctrl_if_);
            if (audio_input_codec_if_ == nullptr || input_control->last_error != ESP_OK)
                return fail(input_control->last_error != ESP_OK ? input_control->last_error
                                                                : ESP_FAIL);
        }

        // Return TX to READY before the codec data interface reconfigures and
        // enables the channels in esp_codec_dev_open(). Keep teardown ownership
        // in the BSP until this bootstrap channel has actually stopped.
        error = i2s_channel_disable(audio_tx_);
        if (error != ESP_OK)
            return fail(error);
        audio_tx_enabled_ = false;

        esp_codec_dev_cfg_t dev_cfg{};
        dev_cfg.dev_type = es8389_capture ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT;
        dev_cfg.codec_if = audio_codec_if_;
        dev_cfg.data_if = audio_data_if_;
        audio_codec_ = esp_codec_dev_new(&dev_cfg);
        if (audio_codec_ == nullptr)
            return fail(ESP_ERR_NO_MEM);

        esp_codec_dev_sample_info_t sample{};
        sample.sample_rate = config.sample_rate_hz;
        sample.channel = config.slot_mode == I2S_SLOT_MODE_MONO ? 1U : 2U;
        sample.channel_mask = config.slot_mode == I2S_SLOT_MODE_MONO
                                  ? static_cast<std::uint16_t>(config.slot_mask)
                                  : 0;
        sample.bits_per_sample = static_cast<std::uint8_t>(config.word_length);
        sample.mclk_multiple = static_cast<int>(config.mclk_multiple);
        error = esp_codec_dev_open(audio_codec_, &sample);
        if (error != ESP_OK)
            return fail(error);
        if (audio_input_codec_if_ != nullptr)
        {
            esp_codec_dev_cfg_t input_cfg{};
            input_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
            input_cfg.codec_if = audio_input_codec_if_;
            input_cfg.data_if = audio_data_if_;
            audio_input_codec_ = esp_codec_dev_new(&input_cfg);
            if (audio_input_codec_ == nullptr)
                return fail(ESP_ERR_NO_MEM);
            error = esp_codec_dev_open(audio_input_codec_, &sample);
            if (error != ESP_OK)
                return fail(error);
        }

        // esp_codec_dev_open() ignores some data-interface/register errors.
        // Check the control adapters and actual I2S state before reporting success.
        error = output_control->last_error;
        if (error == ESP_OK && audio_input_ctrl_if_ != nullptr)
            error = reinterpret_cast<const CodecControl *>(audio_input_ctrl_if_)->last_error;
        i2s_chan_info_t info{};
        if (error == ESP_OK)
            error = i2s_channel_get_info(audio_tx_, &info);
        if (error == ESP_OK && !info.is_enabled)
            error = ESP_ERR_INVALID_STATE;
        if (error == ESP_OK && audio_rx_ != nullptr)
        {
            error = i2s_channel_get_info(audio_rx_, &info);
            if (error == ESP_OK && !info.is_enabled)
                error = ESP_ERR_INVALID_STATE;
        }
        if (error != ESP_OK)
            return fail(error);

        error = esp_codec_dev_set_out_vol(audio_codec_, volumePercent(config.output_volume_db));
        if (error == ESP_OK && config.enable_capture)
        {
            const auto *input_if =
                audio_input_codec_if_ != nullptr ? audio_input_codec_if_ : audio_codec_if_;
            error = input_if->set_mic_gain(input_if, gainDb(config.microphone_gain));
        }
        if (error == ESP_OK)
            error = setDacMuted(audio_ctrl_if_, false);
        // Undo the vendor driver's ADC mute side effects, or keep unused ES8389
        // capture muted when the physical input comes from a separate ADC.
        if (error == ESP_OK)
            error = es8389_capture ? setCaptureMuted(audio_ctrl_if_, false, false)
                                   : configureOutputOnly(audio_ctrl_if_);
        if (error == ESP_OK && audio_input_ctrl_if_ != nullptr)
            error = setCaptureMuted(audio_input_ctrl_if_, true, false);
        if (error == ESP_OK)
            error = output_control->last_error;
        if (error == ESP_OK && audio_input_ctrl_if_ != nullptr)
            error = reinterpret_cast<const CodecControl *>(audio_input_ctrl_if_)->last_error;
        if (error != ESP_OK)
            return fail(error);

        audio_config_ = config;
        s_public_audio_volume_db.store(config.output_volume_db);
        audio_capture_enabled_ = config.enable_capture;
        audio_capture_muted_ = false;
        audio_output_muted_ = false;
        audio_output_enabled_ = config.enable_speaker;
        audio_tx_enabled_ = true;
        audio_rx_enabled_ = config.enable_capture;
        audio_initialized_ = true;
        resetAudioStatistics();
        // Output is permitted by enable_speaker, but PA stays off until playback.
        ESP_LOGI(kTag, "audio initialized: output=ES8389, input=%s, %lu Hz, %u-bit, PA off",
                 !config.enable_capture         ? "disabled"
                 : pins::audioCaptureUsesEs7210 ? "ES7210"
                                                : "ES8389",
                 static_cast<unsigned long>(config.sample_rate_hz),
                 static_cast<unsigned>(config.word_length));
        return ESP_OK;
    }

    esp_err_t Board::deinitializeAudio()
    {
        // Never release a codec or DMA buffer while a BSP worker still uses it.
        esp_err_t error = stopRecording(2000);
        if (error != ESP_OK &&
            (recording_capture_task_ != nullptr || recording_writer_task_ != nullptr))
        {
            (void)setSpeakerEnabled(false);
            return error;
        }
        if (error != ESP_OK)
            ESP_LOGW(kTag, "recording stopped with error before audio teardown: %s",
                     esp_err_to_name(error));
        error = deinitializeMusic();
        if (error != ESP_OK)
        {
            (void)setSpeakerEnabled(false);
            return error;
        }
        const bool has_resources = audio_initialized_ || audio_tx_ != nullptr ||
                                   audio_rx_ != nullptr || audio_codec_ != nullptr ||
                                   audio_input_codec_ != nullptr || audio_codec_if_ != nullptr ||
                                   audio_input_codec_if_ != nullptr || audio_data_if_ != nullptr ||
                                   audio_ctrl_if_ != nullptr || audio_input_ctrl_if_ != nullptr;
        if (!has_resources)
            return ESP_OK;
        esp_err_t first = ESP_OK;
        preserveFirstError(setSpeakerEnabled(false), first);
        if (audio_input_codec_ != nullptr)
        {
            preserveFirstError(esp_codec_dev_close(audio_input_codec_), first);
            esp_codec_dev_delete(audio_input_codec_);
            audio_input_codec_ = nullptr;
        }
        if (audio_codec_ != nullptr)
        {
            preserveFirstError(esp_codec_dev_close(audio_codec_), first);
            esp_codec_dev_delete(audio_codec_);
            audio_codec_ = nullptr;
        }
        for (auto **codec_if : {&audio_input_codec_if_, &audio_codec_if_})
        {
            if (*codec_if != nullptr)
            {
                preserveFirstError(audio_codec_delete_codec_if(*codec_if), first);
                *codec_if = nullptr;
            }
        }
        for (auto **ctrl_if : {&audio_input_ctrl_if_, &audio_ctrl_if_})
        {
            if (*ctrl_if != nullptr)
            {
                preserveFirstError(audio_codec_delete_ctrl_if(*ctrl_if), first);
                *ctrl_if = nullptr;
            }
        }
        if (audio_data_if_ != nullptr)
        {
            preserveFirstError(audio_codec_delete_data_if(audio_data_if_), first);
            audio_data_if_ = nullptr;
        }
        // Query actual channel state: a failure may occur during bootstrap or
        // inside esp_codec_dev_open(), before our enabled flags are published.
        preserveFirstError(releaseChannel(audio_rx_), first);
        preserveFirstError(releaseChannel(audio_tx_), first);
        audio_initialized_ = false;
        audio_tx_enabled_ = false;
        audio_rx_enabled_ = false;
        audio_capture_enabled_ = false;
        audio_output_enabled_ = false;
        audio_capture_muted_ = false;
        audio_output_muted_ = false;
        audio_config_ = AudioConfig{};
        return first;
    }

    esp_err_t Board::reconfigureAudio(const AudioConfig &config)
    {
        i2s_data_bit_width_t width{};
        esp_err_t error = validateConfig(config, width);
        if (error != ESP_OK)
            return error;
        if (!isAudioInitialized())
            return initializeAudio(config);
        if (sameConfig(config, audio_config_))
            return ESP_OK;
        error = deinitializeAudio();
        return error == ESP_OK ? initializeAudio(config) : error;
    }

    bool Board::isAudioInitialized() const noexcept
    {
        return audio_initialized_ && audio_codec_ != nullptr && audio_tx_ != nullptr &&
               audio_tx_enabled_ &&
               (!audio_capture_enabled_ || (audio_rx_ != nullptr && audio_rx_enabled_));
    }

    AudioConfig Board::audioConfig() const noexcept
    {
        return audio_config_;
    }

    AudioStatistics Board::audioStatistics() const noexcept
    {
        AudioStatistics s{};
        s.tx_calls = audio_tx_calls_.load();
        s.rx_calls = audio_rx_calls_.load();
        s.tx_bytes = audio_tx_bytes_.load();
        s.rx_bytes = audio_rx_bytes_.load();
        s.tx_timeouts = audio_tx_timeouts_.load();
        s.rx_timeouts = audio_rx_timeouts_.load();
        s.tx_errors = audio_tx_errors_.load();
        s.rx_errors = audio_rx_errors_.load();
        s.tx_short_writes = audio_tx_short_writes_.load();
        s.rx_short_reads = audio_rx_short_reads_.load();
        return s;
    }

    void Board::resetAudioStatistics() noexcept
    {
        audio_tx_calls_.store(0);
        audio_rx_calls_.store(0);
        audio_tx_bytes_.store(0);
        audio_rx_bytes_.store(0);
        audio_tx_timeouts_.store(0);
        audio_rx_timeouts_.store(0);
        audio_tx_errors_.store(0);
        audio_rx_errors_.store(0);
        audio_tx_short_writes_.store(0);
        audio_rx_short_reads_.store(0);
    }

    esp_err_t Board::readAudio(void *data, std::size_t size, std::size_t &bytes_read,
                               std::uint32_t timeout_ms)
    {
        bytes_read = 0;
        if (!isAudioInitialized() || !audio_capture_enabled_ || audio_rx_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        if (data == nullptr || size == 0U)
            return ESP_ERR_INVALID_ARG;
        ++audio_rx_calls_;
        const esp_err_t error = i2s_channel_read(audio_rx_, data, size, &bytes_read, timeout_ms);
        audio_rx_bytes_.fetch_add(static_cast<std::uint32_t>(bytes_read));
        if (error == ESP_ERR_TIMEOUT)
            ++audio_rx_timeouts_;
        else if (error != ESP_OK)
            ++audio_rx_errors_;
        else if (bytes_read != size)
            ++audio_rx_short_reads_;
        if (error == ESP_OK && audio_capture_muted_ && bytes_read != 0U)
            std::memset(data, 0, bytes_read);
        return error;
    }

    esp_err_t Board::writeAudio(const void *data, std::size_t size, std::size_t &bytes_written,
                                std::uint32_t timeout_ms)
    {
        bytes_written = 0;
        if (!isAudioInitialized() || data == nullptr || size == 0U)
            return !isAudioInitialized() ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
        ++audio_tx_calls_;
        const esp_err_t error =
            i2s_channel_write(audio_tx_, data, size, &bytes_written, timeout_ms);
        audio_tx_bytes_.fetch_add(static_cast<std::uint32_t>(bytes_written));
        if (error == ESP_ERR_TIMEOUT)
            ++audio_tx_timeouts_;
        else if (error != ESP_OK)
            ++audio_tx_errors_;
        else if (bytes_written != size)
            ++audio_tx_short_writes_;
        return error;
    }

    esp_err_t Board::drainAudio(std::uint32_t timeout_ms)
    {
        if (!isAudioInitialized())
            return ESP_ERR_INVALID_STATE;
        // Eight bytes per frame bounds every supported slot/word configuration.
        // More than one full DMA ring of zeros ensures the last PCM has left TX,
        // even though i2s_channel_write() only waits for space in that ring.
        std::size_t remaining =
            (static_cast<std::size_t>(audio_config_.dma_descriptor_count) + 2U) *
            audio_config_.dma_frames_per_descriptor * 8U;
        remaining = (remaining + 23U) / 24U * 24U;
        alignas(4) static constexpr std::uint8_t silence[384]{};
        const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
        while (remaining != 0U)
        {
            const int64_t time_left = deadline - esp_timer_get_time();
            if (time_left <= 0)
                return ESP_ERR_TIMEOUT;
            std::size_t written = 0;
            const esp_err_t error =
                writeAudio(silence, std::min(remaining, sizeof(silence)), written,
                           static_cast<std::uint32_t>((time_left + 999) / 1000));
            remaining -= written;
            if (error != ESP_OK)
                return error;
            if (written == 0U)
                return ESP_ERR_TIMEOUT;
        }
        return ESP_OK;
    }

    esp_err_t Board::setAudioVolumeDb(float db)
    {
        if (!isAudioInitialized() || audio_codec_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        if (!std::isfinite(db) || db < kEs8389MinVolumeDb || db > kEs8389MaxVolumeDb)
            return ESP_ERR_INVALID_ARG;
        const esp_err_t error = esp_codec_dev_set_out_vol(audio_codec_, volumePercent(db));
        if (error == ESP_OK)
        {
            s_public_audio_volume_db.store(db);
            audio_config_.output_volume_db = db;
        }
        return error;
    }

    float Board::audioVolumeDb() const noexcept
    {
        return s_public_audio_volume_db.load();
    }

    esp_err_t Board::setAudioMuted(bool muted)
    {
        if (!isAudioInitialized() || audio_codec_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        const esp_err_t error = setDacMuted(audio_ctrl_if_, muted);
        if (error == ESP_OK)
            audio_output_muted_ = muted;
        return error;
    }

    bool Board::isAudioMuted() const noexcept
    {
        return audio_output_muted_;
    }

    esp_err_t Board::setAudioOutputEnabled(bool enabled)
    {
        if (!isAudioInitialized() || audio_codec_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        if (!enabled)
        {
            esp_err_t error = setDacMuted(audio_ctrl_if_, true);
            // A codec mute failure must not leave the external amplifier on.
            preserveFirstError(setSpeakerEnabled(false), error);
            if (error == ESP_OK)
                audio_output_enabled_ = false;
            return error;
        }
        esp_err_t error = ESP_OK;
        if (!isSpeakerEnabled())
            error = drainAudio();
        if (error == ESP_OK)
            error = setDacMuted(audio_ctrl_if_, audio_output_muted_);
        if (error == ESP_OK && !isSpeakerEnabled())
        {
            error = setSpeakerEnabled(true);
            if (error == ESP_OK)
                vTaskDelay(timeoutTicks(10)); // PA wake-up time; TX is still silent.
        }
        if (error != ESP_OK)
            (void)setSpeakerEnabled(false);
        if (error == ESP_OK)
            audio_output_enabled_ = true;
        return error;
    }

    bool Board::isAudioOutputEnabled() const noexcept
    {
        return audio_output_enabled_ && isSpeakerEnabled();
    }

    esp_err_t Board::setAudioCaptureEnabled(bool enabled)
    {
        if (!isAudioInitialized() || audio_rx_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        if (enabled == audio_capture_enabled_)
            return ESP_OK;
        const esp_err_t error =
            enabled ? i2s_channel_enable(audio_rx_) : i2s_channel_disable(audio_rx_);
        if (error == ESP_OK)
        {
            audio_rx_enabled_ = enabled;
            audio_capture_enabled_ = enabled;
            audio_config_.enable_capture = enabled;
        }
        return error;
    }

    bool Board::isAudioCaptureEnabled() const noexcept
    {
        return isAudioInitialized() && audio_capture_enabled_;
    }

    esp_err_t Board::setAudioCaptureMuted(bool muted)
    {
        if (!isAudioInitialized() || audio_rx_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        const auto *ctrl = audio_input_ctrl_if_ != nullptr ? audio_input_ctrl_if_ : audio_ctrl_if_;
        const esp_err_t error = setCaptureMuted(ctrl, pins::audioCaptureUsesEs7210, muted);
        if (error == ESP_OK)
            audio_capture_muted_ = muted;
        return error;
    }

    bool Board::isAudioCaptureMuted() const noexcept
    {
        return audio_capture_muted_;
    }

    esp_err_t Board::setMicrophoneGain(AudioMicrophoneGain gain, AudioMicrophoneChannel channels)
    {
        if (!isAudioInitialized() || !audio_capture_enabled_ || audio_rx_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        const auto mask = static_cast<std::uint16_t>(channels);
        if (mask == 0 || (mask & ~0x03U) != 0 || !std::isfinite(gainDb(gain)))
            return ESP_ERR_INVALID_ARG;
        const auto *input_if =
            audio_input_codec_if_ != nullptr ? audio_input_codec_if_ : audio_codec_if_;
        const esp_err_t error = input_if->set_mic_channel_gain(input_if, mask, gainDb(gain));
        if (error == ESP_OK)
            audio_config_.microphone_gain = gain;
        return error;
    }

    esp_codec_dev_handle_t Board::audioCodec() noexcept
    {
        return isAudioInitialized() ? audio_codec_ : nullptr;
    }

    esp_codec_dev_handle_t Board::audioInputCodec() noexcept
    {
        if (!isAudioInitialized() || audio_rx_ == nullptr)
            return nullptr;
        return audio_input_codec_ != nullptr ? audio_input_codec_ : audio_codec_;
    }

    i2s_chan_handle_t Board::audioTransmitChannel() const noexcept
    {
        return isAudioInitialized() ? audio_tx_ : nullptr;
    }

    i2s_chan_handle_t Board::audioReceiveChannel() const noexcept
    {
        return isAudioInitialized() && audio_capture_enabled_ ? audio_rx_ : nullptr;
    }

    esp_err_t Board::setMusicVolumePercent(std::int32_t percent)
    {
        percent = std::clamp<std::int32_t>(percent, 0, 100);
        return setAudioVolumeDb(-60.0F + static_cast<float>(percent) * 0.6F);
    }

    std::int32_t Board::musicVolumePercent() const noexcept
    {
        return std::clamp<std::int32_t>(
            static_cast<std::int32_t>((audioVolumeDb() + 60.0F) / 0.6F + 0.5F), 0, 100);
    }

    esp_err_t Board::fadeMusicVolumePercent(std::int32_t target_percent, std::uint32_t duration_ms)
    {
        if (!isAudioInitialized())
            return ESP_ERR_INVALID_STATE;
        target_percent = std::clamp<std::int32_t>(target_percent, 0, 100);
        const std::int32_t start = musicVolumePercent();
        if (duration_ms == 0 || start == target_percent)
            return setMusicVolumePercent(target_percent);
        const std::uint32_t steps = std::max<std::uint32_t>(1U, duration_ms / 20U);
        for (std::uint32_t step = 1; step <= steps; ++step)
        {
            const auto value =
                start + static_cast<std::int32_t>(
                            static_cast<std::int64_t>(target_percent - start) * step / steps);
            const esp_err_t error = setMusicVolumePercent(value);
            if (error != ESP_OK)
                return error;
            vTaskDelay(timeoutTicks(duration_ms / steps));
        }
        return ESP_OK;
    }

} // namespace bsp::T_PANEL_P4_BSP_NAMESPACE
