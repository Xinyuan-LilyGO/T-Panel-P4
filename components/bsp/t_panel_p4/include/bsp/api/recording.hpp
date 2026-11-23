// Board class-body fragments for src/recording.cpp.
// The public and private sections are selected by board.hpp.

#if defined(BSP_BOARD_API_PUBLIC)

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

#elif defined(BSP_BOARD_API_PRIVATE)

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

#endif
