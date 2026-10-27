#include <bsp/t_panel_p4_rect.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "bsp_private.hpp"

namespace
{
    constexpr char kTag[] = "t_panel_p4_audio";
    std::atomic<std::int32_t> s_volume_percent{72};

    void preserveFirstError(esp_err_t candidate, esp_err_t &first_error)
    {
        if (first_error == ESP_OK && candidate != ESP_OK)
            first_error = candidate;
    }

    bool isValidMclkMultiple(i2s_mclk_multiple_t multiple) noexcept
    {
        switch (multiple)
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

    bool toI2sDataWidth(pdl::audio::Es8389WordLength word_length, i2s_data_bit_width_t &data_width) noexcept
    {
        using pdl::audio::Es8389WordLength;
        switch (word_length)
        {
        case Es8389WordLength::bits16:
            data_width = I2S_DATA_BIT_WIDTH_16BIT;
            return true;
        case Es8389WordLength::bits24:
            data_width = I2S_DATA_BIT_WIDTH_24BIT;
            return true;
        case Es8389WordLength::bits32:
            data_width = I2S_DATA_BIT_WIDTH_32BIT;
            return true;
        default:
            return false;
        }
    }

    bool isValidSlotConfig(
        i2s_slot_mode_t slot_mode,
        i2s_std_slot_mask_t slot_mask) noexcept
    {
        if (slot_mode != I2S_SLOT_MODE_MONO &&
            slot_mode != I2S_SLOT_MODE_STEREO)
            return false;
        if (slot_mask == 0)
            return false;
        if (slot_mode == I2S_SLOT_MODE_MONO &&
            slot_mask == I2S_STD_SLOT_BOTH)
            return false;
        return true;
    }

    bool sameAudioConfig(
        const bsp::t_panel_p4_rect::AudioConfig &left,
        const bsp::t_panel_p4_rect::AudioConfig &right) noexcept
    {
        return left.sample_rate_hz == right.sample_rate_hz &&
               left.word_length == right.word_length &&
               left.mclk_multiple == right.mclk_multiple &&
               left.slot_mode == right.slot_mode &&
               left.slot_mask == right.slot_mask &&
               left.dma_descriptor_count == right.dma_descriptor_count &&
               left.dma_frames_per_descriptor == right.dma_frames_per_descriptor &&
               left.output_volume_db == right.output_volume_db &&
               left.microphone_gain == right.microphone_gain &&
               left.no_dac_reference == right.no_dac_reference &&
               left.enable_capture == right.enable_capture &&
               left.enable_speaker == right.enable_speaker;
    }

    esp_err_t validateAudioConfig(
        const bsp::t_panel_p4_rect::AudioConfig &config,
        i2s_data_bit_width_t &data_width) noexcept
    {
        if (config.sample_rate_hz == 0U ||
            config.dma_descriptor_count <= 0 ||
            config.dma_frames_per_descriptor <= 0 ||
            config.output_volume_db < pdl::audio::Es8389::minimumVolumeDb ||
            config.output_volume_db > pdl::audio::Es8389::maximumVolumeDb ||
            !isValidMclkMultiple(config.mclk_multiple) ||
            !isValidSlotConfig(config.slot_mode, config.slot_mask) ||
            !toI2sDataWidth(config.word_length, data_width))
        {
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    TickType_t timeoutTicks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == 0U)
            return 0;
        TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        return ticks == 0 ? 1 : ticks;
    }
}

namespace bsp::t_panel_p4_rect
{
    esp_err_t Board::initializeAudio()
    {
        return initializeAudio(AudioConfig{});
    }

    esp_err_t Board::initializeAudio(const AudioConfig &config)
    {
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (isAudioInitialized())
            return ESP_OK;

        i2s_data_bit_width_t data_width{};
        const esp_err_t config_error = validateAudioConfig(config, data_width);
        if (config_error != ESP_OK)
            return config_error;

        const auto fail = [this](esp_err_t failure)
        {
            const esp_err_t cleanup = deinitializeAudio();
            if (cleanup != ESP_OK)
            {
                ESP_LOGE(
                    kTag,
                    "audio cleanup failed after initialization error: %s",
                    esp_err_to_name(cleanup));
            }
            return failure;
        };

        // Keep the external amplifier muted while clocks and the codec signal
        // path are being configured. It is enabled only after both paths start.
        esp_err_t error = setSpeakerEnabled(false);
        if (error != ESP_OK)
        {
            ESP_LOGE(
                kTag,
                "disable speaker before audio initialization failed: %s",
                esp_err_to_name(error));
            return error;
        }

        i2s_chan_config_t channel_config =
            I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        channel_config.auto_clear = true;
        channel_config.dma_desc_num = config.dma_descriptor_count;
        channel_config.dma_frame_num = config.dma_frames_per_descriptor;
        audio_capture_enabled_ = config.enable_capture;

        error = i2s_new_channel(
            &channel_config,
            &audio_tx_,
            config.enable_capture ? &audio_rx_ : nullptr);
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "create I2S channels failed: %s", esp_err_to_name(error));
            return fail(error);
        }

        i2s_std_config_t rect_config{};
        rect_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config.sample_rate_hz);
        rect_config.clk_cfg.mclk_multiple = config.mclk_multiple;
        rect_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            data_width,
            config.slot_mode);
        rect_config.slot_cfg.slot_mask = config.slot_mask;
        rect_config.gpio_cfg.mclk = pins::i2sMasterClock;
        rect_config.gpio_cfg.bclk = pins::i2sBitClock;
        rect_config.gpio_cfg.ws = pins::i2sWordSelect;
        rect_config.gpio_cfg.dout = pins::i2sDataOut;
        rect_config.gpio_cfg.din = pins::i2sDataIn;
        rect_config.gpio_cfg.invert_flags.mclk_inv = false;
        rect_config.gpio_cfg.invert_flags.bclk_inv = false;
        rect_config.gpio_cfg.invert_flags.ws_inv = false;

        error = i2s_channel_init_std_mode(audio_tx_, &rect_config);
        if (error == ESP_OK && audio_rx_ != nullptr)
            error = i2s_channel_init_std_mode(audio_rx_, &rect_config);
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "configure I2S rect mode failed: %s", esp_err_to_name(error));
            return fail(error);
        }

        es8389_transport_.emplace(
            i2c_bus_,
            pdl::audio::Es8389::defaultI2cAddress,
            pdl::audio::Es8389::defaultI2cFrequencyHz);
        es8389_.emplace(*es8389_transport_, task_delay_);

        pdl::audio::Es8389Config codec_config{};
        codec_config.master_mode = false;
        codec_config.use_mclk = true;
        codec_config.no_dac_reference = config.no_dac_reference;
        codec_config.mclk_divider = static_cast<std::uint16_t>(config.mclk_multiple);
        error = detail::toEspError(es8389_->initialize(codec_config));
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "initialize ES8389 failed: %s", esp_err_to_name(error));
            return fail(error);
        }

        const std::uint64_t mclk_frequency =
            static_cast<std::uint64_t>(config.sample_rate_hz) *
            static_cast<std::uint32_t>(config.mclk_multiple);
        if (mclk_frequency > UINT32_MAX)
            return fail(ESP_ERR_INVALID_ARG);

        pdl::audio::Es8389SampleConfig sample_config{};
        sample_config.sample_rate_hz = config.sample_rate_hz;
        sample_config.word_length = config.word_length;
        sample_config.format = pdl::audio::Es8389SerialFormat::i2s;
        // ES8389 is clocked directly from the external I2S MCLK. In this mode the
        // codec's initialized clock coefficients must be preserved; only the MCLK
        // divider, word length, and serial format are configured.
        sample_config.mclk_frequency_hz = 0;
        error = detail::toEspError(es8389_->configureSample(sample_config));
        if (error == ESP_OK)
            error = detail::toEspError(es8389_->setVolumeDb(config.output_volume_db));
        if (error == ESP_OK)
            error = detail::toEspError(es8389_->setMicrophoneGain(config.microphone_gain));
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "configure ES8389 failed: %s", esp_err_to_name(error));
            return fail(error);
        }

        error = i2s_channel_enable(audio_tx_);
        if (error == ESP_OK)
            audio_tx_enabled_ = true;
        if (error == ESP_OK && audio_rx_ != nullptr)
            error = i2s_channel_enable(audio_rx_);
        if (error == ESP_OK && audio_rx_ != nullptr)
            audio_rx_enabled_ = true;
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "enable I2S channels failed: %s", esp_err_to_name(error));
            return fail(error);
        }

        error = detail::toEspError(es8389_->setEnabled(true));
        if (error == ESP_OK && config.enable_speaker)
            error = setSpeakerEnabled(true);
        if (error != ESP_OK)
        {
            ESP_LOGE(kTag, "start audio path failed: %s", esp_err_to_name(error));
            return fail(error);
        }

        audio_config_ = config;
        audio_volume_db_.store(config.output_volume_db);
        s_volume_percent.store(std::clamp<std::int32_t>(
            static_cast<std::int32_t>((config.output_volume_db + 60.0F) / 0.6F + 0.5F),
            0,
            100));
        audio_capture_muted_ = false;
        audio_output_muted_ = false;
        speaker_enabled_ = config.enable_speaker;
        resetAudioStatistics();
        audio_initialized_ = true;
        audio_output_enabled_ = config.enable_speaker;
        ESP_LOGI(
            kTag,
            "audio initialized: %lu Hz, %u-bit %s, MCLK=%lu Hz, capture=%s, speaker=%s",
            static_cast<unsigned long>(config.sample_rate_hz),
            static_cast<unsigned int>(config.word_length),
            config.slot_mode == I2S_SLOT_MODE_MONO ? "mono" : "stereo",
            static_cast<unsigned long>(mclk_frequency),
            config.enable_capture ? "enabled" : "disabled",
            config.enable_speaker ? "enabled" : "disabled");
        return ESP_OK;
    }

    esp_err_t Board::deinitializeAudio()
    {
        const bool has_resources = audio_initialized_ || audio_tx_ != nullptr ||
                                   audio_rx_ != nullptr || es8389_.has_value() ||
                                   es8389_transport_.has_value();
#ifndef T_PANEL_P4_RECT_NO_AUDIO_FEATURES
        const esp_err_t recording_error = stopRecording(2000);
        if (recording_error != ESP_OK)
        {
            ESP_LOGE(
                kTag,
                "deinitialize recording before audio deinitialization failed: %s",
                esp_err_to_name(recording_error));
            return recording_error;
        }
#endif
#ifndef T_PANEL_P4_RECT_NO_AUDIO_FEATURES
        const esp_err_t music_error = deinitializeMusic();
        if (music_error != ESP_OK)
        {
            ESP_LOGE(
                kTag,
                "deinitialize music before audio deinitialization failed: %s",
                esp_err_to_name(music_error));
            return music_error;
        }
#endif
        if (!has_resources)
            return ESP_OK;

        esp_err_t first_error = ESP_OK;
        if (initialized_)
            preserveFirstError(setSpeakerEnabled(false), first_error);

        if (es8389_ && es8389_->isInitialized())
        {
            if (es8389_->isEnabled())
            {
                preserveFirstError(
                    detail::toEspError(es8389_->setEnabled(false)),
                    first_error);
            }
            preserveFirstError(
                detail::toEspError(es8389_->deinitialize()),
                first_error);
        }
        es8389_.reset();
        es8389_transport_.reset();

        if (audio_rx_enabled_ && audio_rx_ != nullptr)
        {
            const esp_err_t error = i2s_channel_disable(audio_rx_);
            preserveFirstError(error, first_error);
            if (error == ESP_OK)
                audio_rx_enabled_ = false;
        }
        if (audio_tx_enabled_ && audio_tx_ != nullptr)
        {
            const esp_err_t error = i2s_channel_disable(audio_tx_);
            preserveFirstError(error, first_error);
            if (error == ESP_OK)
                audio_tx_enabled_ = false;
        }
        if (audio_rx_ != nullptr && !audio_rx_enabled_)
        {
            const esp_err_t error = i2s_del_channel(audio_rx_);
            preserveFirstError(error, first_error);
            if (error == ESP_OK)
                audio_rx_ = nullptr;
        }
        if (audio_tx_ != nullptr && !audio_tx_enabled_)
        {
            const esp_err_t error = i2s_del_channel(audio_tx_);
            preserveFirstError(error, first_error);
            if (error == ESP_OK)
                audio_tx_ = nullptr;
        }

        audio_initialized_ = false;
        audio_capture_enabled_ = false;
        audio_capture_muted_ = false;
        audio_output_enabled_ = false;
        audio_output_muted_ = false;
        speaker_enabled_ = false;
        audio_config_ = AudioConfig{};
        audio_volume_db_.store(0.0F);
        return first_error;
    }

    esp_err_t Board::reconfigureAudio(const AudioConfig &config)
    {
        i2s_data_bit_width_t data_width{};
        const esp_err_t config_error = validateAudioConfig(config, data_width);
        if (config_error != ESP_OK)
            return config_error;
        if (!initialized_)
            return ESP_ERR_INVALID_STATE;
        if (!isAudioInitialized())
            return initializeAudio(config);
        if (sameAudioConfig(config, audio_config_))
            return ESP_OK;

#ifndef T_PANEL_P4_RECT_NO_AUDIO_FEATURES
        const esp_err_t stop_error = stopMusic(2000);
        if (stop_error != ESP_OK)
            return stop_error;
#endif
        const esp_err_t deinit_error = deinitializeAudio();
        if (deinit_error != ESP_OK)
            return deinit_error;
        return initializeAudio(config);
    }

    bool Board::isAudioInitialized() const noexcept
    {
        return audio_initialized_ && es8389_ && es8389_->isInitialized() &&
               audio_tx_ != nullptr && audio_tx_enabled_ &&
               (!audio_capture_enabled_ ||
                (audio_rx_ != nullptr && audio_rx_enabled_));
    }

    AudioConfig Board::audioConfig() const noexcept
    {
        return audio_config_;
    }

    AudioStatistics Board::audioStatistics() const noexcept
    {
        AudioStatistics statistics{};
        statistics.tx_calls = audio_tx_calls_.load();
        statistics.rx_calls = audio_rx_calls_.load();
        statistics.tx_bytes = audio_tx_bytes_.load();
        statistics.rx_bytes = audio_rx_bytes_.load();
        statistics.tx_timeouts = audio_tx_timeouts_.load();
        statistics.rx_timeouts = audio_rx_timeouts_.load();
        statistics.tx_errors = audio_tx_errors_.load();
        statistics.rx_errors = audio_rx_errors_.load();
        statistics.tx_short_writes = audio_tx_short_writes_.load();
        statistics.rx_short_reads = audio_rx_short_reads_.load();
        return statistics;
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

    esp_err_t Board::readAudio(
        void *data,
        std::size_t size,
        std::size_t &bytes_read,
        std::uint32_t timeout_ms)
    {
        bytes_read = 0;
        if (!isAudioInitialized() || !audio_capture_enabled_ || audio_rx_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        if (data == nullptr || size == 0U)
            return ESP_ERR_INVALID_ARG;
        ++audio_rx_calls_;
        const esp_err_t error = i2s_channel_read(
            audio_rx_,
            data,
            size,
            &bytes_read,
            timeoutTicks(timeout_ms));
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

    esp_err_t Board::writeAudio(
        const void *data,
        std::size_t size,
        std::size_t &bytes_written,
        std::uint32_t timeout_ms)
    {
        bytes_written = 0;
        if (!isAudioInitialized())
            return ESP_ERR_INVALID_STATE;
        if (data == nullptr || size == 0U)
            return ESP_ERR_INVALID_ARG;
        ++audio_tx_calls_;
        const esp_err_t error = i2s_channel_write(
            audio_tx_,
            data,
            size,
            &bytes_written,
            timeoutTicks(timeout_ms));
        audio_tx_bytes_.fetch_add(static_cast<std::uint32_t>(bytes_written));
        if (error == ESP_ERR_TIMEOUT)
            ++audio_tx_timeouts_;
        else if (error != ESP_OK)
            ++audio_tx_errors_;
        else if (bytes_written != size)
            ++audio_tx_short_writes_;
        return error;
    }

    esp_err_t Board::setAudioVolumeDb(float decibels)
    {
        if (!isAudioInitialized() || !es8389_)
            return ESP_ERR_INVALID_STATE;
        if (decibels < pdl::audio::Es8389::minimumVolumeDb ||
            decibels > pdl::audio::Es8389::maximumVolumeDb)
            return ESP_ERR_INVALID_ARG;

        const esp_err_t error = detail::toEspError(
            es8389_->setVolumeDb(decibels));
        if (error == ESP_OK)
        {
            audio_volume_db_.store(decibels);
            audio_config_.output_volume_db = decibels;
            s_volume_percent.store(std::clamp<std::int32_t>(
                static_cast<std::int32_t>((decibels + 60.0F) / 0.6F + 0.5F),
                0,
                100));
        }
        return error;
    }

    float Board::audioVolumeDb() const noexcept
    {
        return audio_volume_db_.load();
    }

    esp_err_t Board::setAudioMuted(bool muted)
    {
        if (!isAudioInitialized() || !es8389_)
            return ESP_ERR_INVALID_STATE;
        const esp_err_t error = detail::toEspError(
            es8389_->setMuted(muted));
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
        if (!isAudioInitialized() || !es8389_)
            return ESP_ERR_INVALID_STATE;

        if (!enabled)
        {
            esp_err_t error = detail::toEspError(es8389_->setMuted(true));
            if (error != ESP_OK)
                return error;
            error = setSpeakerEnabled(false);
            if (error != ESP_OK)
                return error;
            audio_output_enabled_ = false;
            return ESP_OK;
        }

        esp_err_t error = setSpeakerEnabled(true);
        if (error != ESP_OK)
            return error;
        error = detail::toEspError(es8389_->setMuted(audio_output_muted_));
        if (error != ESP_OK)
        {
            (void)setSpeakerEnabled(false);
            return error;
        }
        audio_output_enabled_ = true;
        return ESP_OK;
    }

    bool Board::isAudioOutputEnabled() const noexcept
    {
        return audio_output_enabled_ && speaker_enabled_;
    }

    esp_err_t Board::setAudioCaptureEnabled(bool enabled)
    {
        if (!isAudioInitialized())
            return ESP_ERR_INVALID_STATE;
        if (audio_rx_ == nullptr)
            return ESP_ERR_NOT_SUPPORTED;
        if (enabled == audio_capture_enabled_)
            return ESP_OK;

        const esp_err_t error = enabled
                                    ? i2s_channel_enable(audio_rx_)
                                    : i2s_channel_disable(audio_rx_);
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
        audio_capture_muted_ = muted;
        return ESP_OK;
    }

    bool Board::isAudioCaptureMuted() const noexcept
    {
        return audio_capture_muted_;
    }

    esp_err_t Board::setMicrophoneGain(
        pdl::audio::Es8389MicrophoneGain gain,
        pdl::audio::Es8389MicrophoneChannel channels)
    {
        if (!isAudioInitialized() || !es8389_)
            return ESP_ERR_INVALID_STATE;
        const esp_err_t error = detail::toEspError(
            es8389_->setMicrophoneGain(gain, channels));
        if (error == ESP_OK)
            audio_config_.microphone_gain = gain;
        return error;
    }

    pdl::audio::Es8389 *Board::audioCodec() noexcept
    {
        return isAudioInitialized() && es8389_ ? &*es8389_ : nullptr;
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
        const esp_err_t error = setAudioVolumeDb(-60.0F + percent * 0.6F);
        if (error == ESP_OK)
            s_volume_percent.store(percent);
        return error;
    }

    std::int32_t Board::musicVolumePercent() const noexcept
    {
        return s_volume_percent.load();
    }

    esp_err_t Board::fadeMusicVolumePercent(
        std::int32_t target_percent,
        std::uint32_t duration_ms)
    {
        if (!isAudioInitialized())
            return ESP_ERR_INVALID_STATE;
        target_percent = std::clamp<std::int32_t>(target_percent, 0, 100);
        const std::int32_t start_percent = musicVolumePercent();
        if (duration_ms == 0U || start_percent == target_percent)
            return setMusicVolumePercent(target_percent);

        const std::uint32_t steps = std::max<std::uint32_t>(1U, duration_ms / 20U);
        for (std::uint32_t step = 1U; step <= steps; ++step)
        {
            const auto value = start_percent +
                static_cast<std::int32_t>(
                    (static_cast<std::int64_t>(target_percent - start_percent) * step) /
                    steps);
            const esp_err_t error = setMusicVolumePercent(value);
            if (error != ESP_OK)
                return error;
            vTaskDelay(pdMS_TO_TICKS(duration_ms / steps));
        }
        return setMusicVolumePercent(target_percent);
    }
}
