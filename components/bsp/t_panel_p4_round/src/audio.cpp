#include <bsp/t_panel_p4_round.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <es8389_codec.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace
{
constexpr char kTag[] = "t_panel_p4_audio";
constexpr float kEs8389MinVolumeDb = -95.5F;
constexpr float kEs8389MaxVolumeDb = 32.0F;

void preserveFirstError(esp_err_t candidate, esp_err_t &first) noexcept
{
    if (first == ESP_OK && candidate != ESP_OK)
        first = candidate;
}

TickType_t timeoutTicks(std::uint32_t timeout_ms) noexcept
{
    if (timeout_ms == 0U)
        return 0;
    const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    return ticks == 0 ? 1 : ticks;
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

bool toI2sWidth(
    bsp::t_panel_p4_round::AudioWordLength value,
    i2s_data_bit_width_t &width) noexcept
{
    using bsp::t_panel_p4_round::AudioWordLength;
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
    return (mode == I2S_SLOT_MODE_MONO || mode == I2S_SLOT_MODE_STEREO) &&
           mask != 0 && !(mode == I2S_SLOT_MODE_MONO && mask == I2S_STD_SLOT_BOTH);
}

std::uint8_t volumePercent(float db) noexcept
{
    const float normalized = (db - kEs8389MinVolumeDb) /
                             (kEs8389MaxVolumeDb - kEs8389MinVolumeDb);
    return static_cast<std::uint8_t>(std::clamp(normalized * 100.0F, 0.0F, 100.0F) + 0.5F);
}

float gainDb(bsp::t_panel_p4_round::AudioMicrophoneGain gain) noexcept
{
    using G = bsp::t_panel_p4_round::AudioMicrophoneGain;
    switch (gain)
    {
    case G::db0: return 0.0F;
    case G::db3_5: return 3.5F;
    case G::db6_5: return 6.5F;
    case G::db9_5: return 9.5F;
    case G::db12_5: return 12.5F;
    case G::db15_5: return 15.5F;
    case G::db18_5: return 18.5F;
    case G::db21_5: return 21.5F;
    case G::db24_5: return 24.5F;
    case G::db27_5: return 27.5F;
    case G::db30_5: return 30.5F;
    case G::db33_5: return 33.5F;
    case G::db36_5: return 36.5F;
    default: return 0.0F;
    }
}

bool sameConfig(
    const bsp::t_panel_p4_round::AudioConfig &a,
    const bsp::t_panel_p4_round::AudioConfig &b) noexcept
{
    return a.codec == b.codec && a.sample_rate_hz == b.sample_rate_hz &&
           a.word_length == b.word_length && a.mclk_multiple == b.mclk_multiple &&
           a.slot_mode == b.slot_mode && a.slot_mask == b.slot_mask &&
           a.dma_descriptor_count == b.dma_descriptor_count &&
           a.dma_frames_per_descriptor == b.dma_frames_per_descriptor &&
           a.output_volume_db == b.output_volume_db &&
           a.microphone_gain == b.microphone_gain &&
           a.no_dac_reference == b.no_dac_reference &&
           a.enable_capture == b.enable_capture && a.enable_speaker == b.enable_speaker;
}

esp_err_t validateConfig(
    const bsp::t_panel_p4_round::AudioConfig &config,
    i2s_data_bit_width_t &width) noexcept
{
    if (config.codec != bsp::t_panel_p4_round::AudioCodecChip::es8389 ||
        config.sample_rate_hz == 0U || config.dma_descriptor_count <= 0 ||
        config.dma_frames_per_descriptor <= 0 ||
        config.output_volume_db < kEs8389MinVolumeDb ||
        config.output_volume_db > kEs8389MaxVolumeDb ||
        !validMclk(config.mclk_multiple) ||
        !validSlots(config.slot_mode, config.slot_mask) ||
        !toI2sWidth(config.word_length, width))
        return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}
}

namespace bsp::t_panel_p4_round
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

    i2s_data_bit_width_t width{};
    esp_err_t error = validateConfig(config, width);
    if (error != ESP_OK)
        return error;

    const auto fail = [this](esp_err_t cause) {
        (void)deinitializeAudio();
        return cause;
    };

    error = setSpeakerEnabled(false);
    if (error != ESP_OK)
        return error;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = config.dma_descriptor_count;
    chan_cfg.dma_frame_num = config.dma_frames_per_descriptor;
    error = i2s_new_channel(&chan_cfg, &audio_tx_, config.enable_capture ? &audio_rx_ : nullptr);
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

    audio_codec_i2c_cfg_t ctrl_cfg{};
    ctrl_cfg.port = I2C_NUM_0;
    ctrl_cfg.addr = ES8389_CODEC_DEFAULT_ADDR;
    ctrl_cfg.bus_handle = i2c_bus_.nativeHandle();
    audio_ctrl_if_ = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    if (audio_ctrl_if_ == nullptr)
        return fail(ESP_ERR_NO_MEM);

    es8389_codec_cfg_t es_cfg{};
    es_cfg.ctrl_if = audio_ctrl_if_;
    es_cfg.codec_mode = config.enable_capture ? ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC;
    es_cfg.pa_pin = -1;
    es_cfg.master_mode = false;
    es_cfg.use_mclk = true;
    es_cfg.no_dac_ref = config.no_dac_reference;
    es_cfg.mclk_div = static_cast<std::uint16_t>(config.mclk_multiple);
    audio_codec_if_ = es8389_codec_new(&es_cfg);
    if (audio_codec_if_ == nullptr)
        return fail(ESP_FAIL);

    esp_codec_dev_cfg_t dev_cfg{};
    dev_cfg.dev_type = config.enable_capture ? ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT;
    dev_cfg.codec_if = audio_codec_if_;
    dev_cfg.data_if = audio_data_if_;
    audio_codec_ = esp_codec_dev_new(&dev_cfg);
    if (audio_codec_ == nullptr)
        return fail(ESP_ERR_NO_MEM);

    esp_codec_dev_sample_info_t sample{};
    sample.sample_rate = config.sample_rate_hz;
    sample.channel = config.slot_mode == I2S_SLOT_MODE_MONO ? 1U : 2U;
    sample.channel_mask = 0;
    sample.bits_per_sample = static_cast<std::uint8_t>(config.word_length);
    sample.mclk_multiple = static_cast<int>(config.mclk_multiple);
    error = esp_codec_dev_open(audio_codec_, &sample);
    if (error != ESP_OK)
        return fail(error);
    error = esp_codec_dev_set_out_vol(audio_codec_, volumePercent(config.output_volume_db));
    if (error == ESP_OK && config.enable_capture)
        error = esp_codec_dev_set_in_gain(audio_codec_, gainDb(config.microphone_gain));
    if (error != ESP_OK)
        return fail(error);

    audio_config_ = config;
    audio_volume_db_.store(config.output_volume_db);
    audio_capture_enabled_ = config.enable_capture;
    audio_capture_muted_ = false;
    audio_output_muted_ = false;
    audio_output_enabled_ = config.enable_speaker;
    audio_tx_enabled_ = true;
    audio_rx_enabled_ = config.enable_capture;
    audio_initialized_ = true;
    resetAudioStatistics();
    if (config.enable_speaker)
    {
        error = setSpeakerEnabled(true);
        if (error != ESP_OK)
            return fail(error);
    }
    ESP_LOGI(kTag, "audio initialized: codec=ES8389, %lu Hz, %u-bit, capture=%s",
             static_cast<unsigned long>(config.sample_rate_hz),
             static_cast<unsigned>(config.word_length),
             config.enable_capture ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t Board::deinitializeAudio()
{
    const bool has_resources = audio_initialized_ || audio_tx_ != nullptr || audio_rx_ != nullptr ||
                               audio_codec_ != nullptr || audio_data_if_ != nullptr || audio_ctrl_if_ != nullptr;
    if (!has_resources)
        return ESP_OK;
    esp_err_t first = ESP_OK;
    // Stop the BSP music worker before releasing the codec and I2S handles.
    preserveFirstError(deinitializeMusic(), first);
    if (initialized_)
        preserveFirstError(setSpeakerEnabled(false), first);
    if (audio_codec_ != nullptr)
    {
        preserveFirstError(esp_codec_dev_close(audio_codec_), first);
        // esp_codec_dev_close() disables the data interface and therefore both
        // I2S channels before the handles are released below.
        audio_tx_enabled_ = false;
        audio_rx_enabled_ = false;
        esp_codec_dev_delete(audio_codec_);
        audio_codec_ = nullptr;
    }
    if (audio_codec_if_ != nullptr)
    {
        preserveFirstError(audio_codec_delete_codec_if(audio_codec_if_), first);
        audio_codec_if_ = nullptr;
    }
    if (audio_gpio_if_ != nullptr)
    {
        preserveFirstError(audio_codec_delete_gpio_if(audio_gpio_if_), first);
        audio_gpio_if_ = nullptr;
    }
    if (audio_ctrl_if_ != nullptr)
    {
        preserveFirstError(audio_codec_delete_ctrl_if(audio_ctrl_if_), first);
        audio_ctrl_if_ = nullptr;
    }
    if (audio_data_if_ != nullptr)
    {
        preserveFirstError(audio_codec_delete_data_if(audio_data_if_), first);
        audio_data_if_ = nullptr;
    }
    if (audio_rx_ != nullptr)
    {
        if (audio_rx_enabled_)
            preserveFirstError(i2s_channel_disable(audio_rx_), first);
        preserveFirstError(i2s_del_channel(audio_rx_), first);
        audio_rx_ = nullptr;
    }
    if (audio_tx_ != nullptr)
    {
        if (audio_tx_enabled_)
            preserveFirstError(i2s_channel_disable(audio_tx_), first);
        preserveFirstError(i2s_del_channel(audio_tx_), first);
        audio_tx_ = nullptr;
    }
    audio_initialized_ = false;
    audio_tx_enabled_ = false;
    audio_rx_enabled_ = false;
    audio_capture_enabled_ = false;
    audio_output_enabled_ = false;
    audio_capture_muted_ = false;
    audio_output_muted_ = false;
    speaker_enabled_ = false;
    audio_config_ = AudioConfig{};
    audio_volume_db_.store(0.0F);
    return first;
}

esp_err_t Board::reconfigureAudio(const AudioConfig &config)
{
    i2s_data_bit_width_t width{};
    esp_err_t error = validateConfig(config, width);
    if (error != ESP_OK)
        return error;
    if (!initialized_)
        return ESP_ERR_INVALID_STATE;
    if (!isAudioInitialized())
        return initializeAudio(config);
    if (sameConfig(config, audio_config_))
        return ESP_OK;
    error = deinitializeAudio();
    return error == ESP_OK ? initializeAudio(config) : error;
}

bool Board::isAudioInitialized() const noexcept
{
    return audio_initialized_ && audio_codec_ != nullptr && audio_tx_ != nullptr && audio_tx_enabled_ &&
           (!audio_capture_enabled_ || (audio_rx_ != nullptr && audio_rx_enabled_));
}

AudioConfig Board::audioConfig() const noexcept { return audio_config_; }

AudioStatistics Board::audioStatistics() const noexcept
{
    AudioStatistics s{};
    s.tx_calls = audio_tx_calls_.load(); s.rx_calls = audio_rx_calls_.load();
    s.tx_bytes = audio_tx_bytes_.load(); s.rx_bytes = audio_rx_bytes_.load();
    s.tx_timeouts = audio_tx_timeouts_.load(); s.rx_timeouts = audio_rx_timeouts_.load();
    s.tx_errors = audio_tx_errors_.load(); s.rx_errors = audio_rx_errors_.load();
    s.tx_short_writes = audio_tx_short_writes_.load(); s.rx_short_reads = audio_rx_short_reads_.load();
    return s;
}

void Board::resetAudioStatistics() noexcept
{
    audio_tx_calls_.store(0); audio_rx_calls_.store(0); audio_tx_bytes_.store(0); audio_rx_bytes_.store(0);
    audio_tx_timeouts_.store(0); audio_rx_timeouts_.store(0); audio_tx_errors_.store(0); audio_rx_errors_.store(0);
    audio_tx_short_writes_.store(0); audio_rx_short_reads_.store(0);
}

esp_err_t Board::readAudio(void *data, std::size_t size, std::size_t &bytes_read, std::uint32_t timeout_ms)
{
    bytes_read = 0;
    if (!isAudioInitialized() || !audio_capture_enabled_ || audio_rx_ == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (data == nullptr || size == 0U)
        return ESP_ERR_INVALID_ARG;
    ++audio_rx_calls_;
    const esp_err_t error = i2s_channel_read(audio_rx_, data, size, &bytes_read, timeoutTicks(timeout_ms));
    audio_rx_bytes_.fetch_add(static_cast<std::uint32_t>(bytes_read));
    if (error == ESP_ERR_TIMEOUT) ++audio_rx_timeouts_;
    else if (error != ESP_OK) ++audio_rx_errors_;
    else if (bytes_read != size) ++audio_rx_short_reads_;
    if (error == ESP_OK && audio_capture_muted_ && bytes_read != 0U)
        std::memset(data, 0, bytes_read);
    return error;
}

esp_err_t Board::writeAudio(const void *data, std::size_t size, std::size_t &bytes_written, std::uint32_t timeout_ms)
{
    bytes_written = 0;
    if (!isAudioInitialized() || data == nullptr || size == 0U)
        return !isAudioInitialized() ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    ++audio_tx_calls_;
    const esp_err_t error = i2s_channel_write(audio_tx_, data, size, &bytes_written, timeoutTicks(timeout_ms));
    audio_tx_bytes_.fetch_add(static_cast<std::uint32_t>(bytes_written));
    if (error == ESP_ERR_TIMEOUT) ++audio_tx_timeouts_;
    else if (error != ESP_OK) ++audio_tx_errors_;
    else if (bytes_written != size) ++audio_tx_short_writes_;
    return error;
}

esp_err_t Board::setAudioVolumeDb(float db)
{
    if (!isAudioInitialized() || audio_codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!std::isfinite(db) || db < kEs8389MinVolumeDb || db > kEs8389MaxVolumeDb) return ESP_ERR_INVALID_ARG;
    const esp_err_t error = esp_codec_dev_set_out_vol(audio_codec_, volumePercent(db));
    if (error == ESP_OK) { audio_volume_db_.store(db); audio_config_.output_volume_db = db; }
    return error;
}

float Board::audioVolumeDb() const noexcept { return audio_volume_db_.load(); }

esp_err_t Board::setAudioMuted(bool muted)
{
    if (!isAudioInitialized() || audio_codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = esp_codec_dev_set_out_mute(audio_codec_, muted);
    if (error == ESP_OK) audio_output_muted_ = muted;
    return error;
}

bool Board::isAudioMuted() const noexcept { return audio_output_muted_; }

esp_err_t Board::setAudioOutputEnabled(bool enabled)
{
    if (!isAudioInitialized() || audio_codec_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!enabled)
    {
        esp_err_t error = esp_codec_dev_set_out_mute(audio_codec_, true);
        if (error == ESP_OK) error = setSpeakerEnabled(false);
        if (error == ESP_OK) audio_output_enabled_ = false;
        return error;
    }
    esp_err_t error = setSpeakerEnabled(true);
    if (error == ESP_OK) error = esp_codec_dev_set_out_mute(audio_codec_, audio_output_muted_);
    if (error == ESP_OK) audio_output_enabled_ = true;
    return error;
}

bool Board::isAudioOutputEnabled() const noexcept { return audio_output_enabled_ && speaker_enabled_; }

esp_err_t Board::setAudioCaptureEnabled(bool enabled)
{
    if (!isAudioInitialized() || audio_rx_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (enabled == audio_capture_enabled_) return ESP_OK;
    const esp_err_t error = enabled ? i2s_channel_enable(audio_rx_) : i2s_channel_disable(audio_rx_);
    if (error == ESP_OK) { audio_rx_enabled_ = enabled; audio_capture_enabled_ = enabled; audio_config_.enable_capture = enabled; }
    return error;
}

bool Board::isAudioCaptureEnabled() const noexcept { return isAudioInitialized() && audio_capture_enabled_; }

esp_err_t Board::setAudioCaptureMuted(bool muted)
{
    if (!isAudioInitialized() || audio_rx_ == nullptr) return ESP_ERR_INVALID_STATE;
    audio_capture_muted_ = muted;
    return ESP_OK;
}

bool Board::isAudioCaptureMuted() const noexcept { return audio_capture_muted_; }

esp_err_t Board::setMicrophoneGain(AudioMicrophoneGain gain, AudioMicrophoneChannel channels)
{
    if (!isAudioInitialized() || audio_codec_ == nullptr || !audio_capture_enabled_) return ESP_ERR_INVALID_STATE;
    const esp_err_t error = esp_codec_dev_set_in_channel_gain(
        audio_codec_, static_cast<std::uint16_t>(channels), gainDb(gain));
    if (error == ESP_OK) audio_config_.microphone_gain = gain;
    return error;
}

esp_codec_dev_handle_t Board::audioCodec() noexcept
{
    return isAudioInitialized() ? audio_codec_ : nullptr;
}

i2s_chan_handle_t Board::audioTransmitChannel() const noexcept
{
    return isAudioInitialized() ? audio_tx_ : nullptr;
}

i2s_chan_handle_t Board::audioReceiveChannel() const noexcept
{
    return isAudioInitialized() && audio_capture_enabled_ ? audio_rx_ : nullptr;
}
}
