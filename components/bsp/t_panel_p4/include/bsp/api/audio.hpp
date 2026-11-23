// Board class-body fragments for src/audio.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

// 3.7 音频录放、增益与统计 API：src/audio.cpp
// ========================================================================
[[nodiscard]] esp_err_t initializeAudio();
[[nodiscard]] esp_err_t initializeAudio(const AudioConfig &config);
[[nodiscard]] esp_err_t reconfigureAudio(const AudioConfig &config);
[[nodiscard]] esp_err_t deinitializeAudio();
[[nodiscard]] bool isAudioInitialized() const noexcept;
[[nodiscard]] AudioConfig audioConfig() const noexcept;
// Active capture channels: 0 if disabled, 3 with AEC reference, otherwise 1/2.
[[nodiscard]] std::uint8_t audioCaptureChannelCount() const noexcept;
[[nodiscard]] AudioStatistics audioStatistics() const noexcept;
void resetAudioStatistics() noexcept;
// With enable_aec_reference: interleaved int16_t MIC1, MIC2, MIC3 reference.
// In that mode size must be a multiple of 6 bytes (one complete frame).
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

// Music volume is backed by the shared audio codec path (src/audio.cpp).
[[nodiscard]] esp_err_t setMusicVolumePercent(std::int32_t percent);
[[nodiscard]] esp_err_t fadeMusicVolumePercent(std::int32_t target_percent,
                                               std::uint32_t duration_ms);
[[nodiscard]] std::int32_t musicVolumePercent() const noexcept;

// ========================================================================
// 3.13 音频编解码器与 I2S 句柄：src/audio.cpp
// ========================================================================
// Output handle; also owns ES8389 capture when no separate ADC is fitted.
[[nodiscard]] esp_codec_dev_handle_t audioCodec() noexcept;
[[nodiscard]] esp_codec_dev_handle_t audioInputCodec() noexcept;
[[nodiscard]] i2s_chan_handle_t audioTransmitChannel() const noexcept;
[[nodiscard]] i2s_chan_handle_t audioReceiveChannel() const noexcept;

#elif defined(BSP_BOARD_API_PRIVATE)

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

#endif
