#include <bsp/board.hpp>

#include <array>
#include <cstring>
#include <limits>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

namespace
{
    constexpr std::size_t kWavHeaderBytes = 44U;
    constexpr std::size_t kMinimumChunkBytes = 256U;
    constexpr std::size_t kMaximumChunkBytes = 64U * 1024U;
    constexpr std::size_t kMinimumRingBytes = 2U * kMinimumChunkBytes;
    constexpr std::uint32_t kRecordingMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    StreamBufferHandle_t createRecordingStream(std::size_t capacity)
    {
        // Keep synchronization metadata internal; only PCM storage needs PSRAM.
        auto *control = static_cast<StaticStreamBuffer_t *>(heap_caps_calloc(
            1U, sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        // Static stream buffers reserve one byte to distinguish full from empty.
        auto *storage =
            static_cast<std::uint8_t *>(heap_caps_malloc(capacity + 1U, kRecordingMemoryCaps));
        if (control == nullptr || storage == nullptr)
        {
            heap_caps_free(control);
            heap_caps_free(storage);
            return nullptr;
        }
        StreamBufferHandle_t stream =
            xStreamBufferCreateStatic(capacity + 1U, 1U, storage, control);
        if (stream == nullptr)
        {
            heap_caps_free(control);
            heap_caps_free(storage);
        }
        return stream;
    }

    void deleteRecordingStream(StreamBufferHandle_t stream)
    {
        std::uint8_t *storage = nullptr;
        StaticStreamBuffer_t *control = nullptr;
        const BaseType_t found = xStreamBufferGetStaticBuffers(stream, &storage, &control);
        configASSERT(found == pdTRUE);
        (void)found;
        vStreamBufferDelete(stream);
        heap_caps_free(storage);
        heap_caps_free(control);
    }

    std::uint16_t bitsPerSample(bsp::T_PANEL_P4_BSP_NAMESPACE::AudioWordLength word_length) noexcept
    {
        return static_cast<std::uint16_t>(word_length);
    }

    std::uint16_t channelCount(const bsp::T_PANEL_P4_BSP_NAMESPACE::AudioConfig &config) noexcept
    {
        return config.slot_mode == I2S_SLOT_MODE_MONO ? 1U : 2U;
    }

    void putLe16(std::uint8_t *destination, std::uint16_t value) noexcept
    {
        destination[0] = static_cast<std::uint8_t>(value & 0xffU);
        destination[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    }

    void putLe32(std::uint8_t *destination, std::uint32_t value) noexcept
    {
        destination[0] = static_cast<std::uint8_t>(value & 0xffU);
        destination[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        destination[2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        destination[3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    }

    bool writeWavHeader(FILE *file, std::uint32_t sample_rate, std::uint16_t channels,
                        std::uint16_t bits_per_sample, std::uint32_t data_bytes)
    {
        if (file == nullptr || channels == 0U || bits_per_sample == 0U || sample_rate == 0U)
            return false;

        const std::uint32_t block_align =
            static_cast<std::uint32_t>(channels) * bits_per_sample / 8U;
        const std::uint64_t byte_rate64 = static_cast<std::uint64_t>(sample_rate) * block_align;
        if (block_align == 0U || byte_rate64 > UINT32_MAX || data_bytes > UINT32_MAX - 36U)
            return false;

        std::array<std::uint8_t, kWavHeaderBytes> header{};
        std::memcpy(header.data(), "RIFF", 4);
        putLe32(header.data() + 4U, 36U + data_bytes);
        std::memcpy(header.data() + 8U, "WAVE", 4);
        std::memcpy(header.data() + 12U, "fmt ", 4);
        putLe32(header.data() + 16U, 16U);
        putLe16(header.data() + 20U, 1U);
        putLe16(header.data() + 22U, channels);
        putLe32(header.data() + 24U, sample_rate);
        putLe32(header.data() + 28U, static_cast<std::uint32_t>(byte_rate64));
        putLe16(header.data() + 32U, static_cast<std::uint16_t>(block_align));
        putLe16(header.data() + 34U, bits_per_sample);
        std::memcpy(header.data() + 36U, "data", 4);
        putLe32(header.data() + 40U, data_bytes);

        if (std::fseek(file, 0, SEEK_SET) != 0 ||
            std::fwrite(header.data(), 1, header.size(), file) != header.size())
            return false;
        return std::fflush(file) == 0;
    }

    TickType_t timeoutTicks(std::uint32_t timeout_ms) noexcept
    {
        if (timeout_ms == 0U)
            return 0;
        const TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        return ticks == 0 ? 1 : ticks;
    }
} // namespace

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    void Board::notifyRecordingEvent(RecordingEventType type, RecordingState state, esp_err_t error,
                                     const std::uint8_t *data, std::size_t data_size)
    {
        RecordingEvent event{};
        event.type = type;
        event.state = state;
        event.error = error;
        event.bytes_recorded = recording_bytes_.load();
        const std::int64_t started_us = recording_started_us_.load();
        if (started_us > 0)
        {
            const std::int64_t elapsed_us = esp_timer_get_time() - started_us;
            event.duration_ms =
                elapsed_us <= 0 ? 0U : static_cast<std::uint32_t>(elapsed_us / 1000LL);
        }
        event.data = data;
        event.data_size = data_size;

        const RecordingEventCallback callback = recording_callback_.load();
        if (callback != nullptr)
            callback(event, recording_callback_context_.load());
    }

    void Board::recordingCaptureTaskEntry(void *argument)
    {
        auto *board = static_cast<Board *>(argument);
        if (board == nullptr)
        {
            vTaskDeleteWithCaps(nullptr);
            return;
        }

        const std::size_t chunk_bytes = board->recording_chunk_bytes_;
        std::uint8_t *buffer = board->recording_capture_buffer_;

        board->recording_state_.store(RecordingState::recording);
        board->notifyRecordingEvent(RecordingEventType::state_changed, RecordingState::recording);

        while (!board->recording_stop_requested_.load())
        {
            std::size_t bytes_read = 0U;
            const esp_err_t error = board->readAudio(buffer, chunk_bytes, bytes_read,
                                                     board->recording_read_timeout_ms_);
            if (error == ESP_ERR_TIMEOUT)
                continue;
            if (error != ESP_OK)
            {
                board->recording_last_error_.store(error);
                board->recording_stop_requested_.store(true);
                break;
            }
            if (bytes_read == 0U)
                continue;

            const std::size_t sent = xStreamBufferSend(board->recording_stream_buffer_, buffer,
                                                       bytes_read, timeoutTicks(100U));
            if (sent != bytes_read)
            {
                board->recording_last_error_.store(ESP_ERR_NO_MEM);
                board->recording_stop_requested_.store(true);
                break;
            }
        }

        heap_caps_free(buffer);
        board->recording_capture_buffer_ = nullptr;
        board->recording_capture_task_ = nullptr;
        board->recording_capture_done_.store(true);
        vTaskDeleteWithCaps(nullptr);
    }

    void Board::recordingWriterTaskEntry(void *argument)
    {
        auto *board = static_cast<Board *>(argument);
        if (board == nullptr)
        {
            vTaskDeleteWithCaps(nullptr);
            return;
        }

        const std::size_t chunk_bytes = board->recording_chunk_bytes_;
        std::uint8_t *buffer = board->recording_writer_buffer_;
        bool write_failed = false;

        while (!board->recording_capture_done_.load() ||
               xStreamBufferBytesAvailable(board->recording_stream_buffer_) != 0U)
        {
            const std::size_t received = xStreamBufferReceive(
                board->recording_stream_buffer_, buffer, chunk_bytes, timeoutTicks(100U));
            if (received == 0U)
                continue;

            if (!write_failed &&
                (board->recording_file_ == nullptr ||
                 std::fwrite(buffer, 1, received, board->recording_file_) != received))
            {
                board->recording_last_error_.store(ESP_FAIL);
                board->recording_stop_requested_.store(true);
                write_failed = true;
            }

            if (write_failed)
                continue;

            const std::uint32_t previous = board->recording_bytes_.load();
            const std::uint32_t updated = previous > UINT32_MAX - received
                                              ? UINT32_MAX
                                              : previous + static_cast<std::uint32_t>(received);
            board->recording_bytes_.store(updated);
            board->notifyRecordingEvent(RecordingEventType::data, RecordingState::recording, ESP_OK,
                                        buffer, received);
        }

        if (board->recording_file_ != nullptr)
        {
            const bool header_ok = writeWavHeader(
                board->recording_file_, board->recording_sample_rate_, board->recording_channels_,
                board->recording_bits_per_sample_, board->recording_bytes_.load());
            if (!header_ok && board->recording_last_error_.load() == ESP_OK)
                board->recording_last_error_.store(ESP_FAIL);
            std::fclose(board->recording_file_);
            board->recording_file_ = nullptr;
        }

        if (board->recording_stream_buffer_ != nullptr)
        {
            deleteRecordingStream(board->recording_stream_buffer_);
            board->recording_stream_buffer_ = nullptr;
        }
        heap_caps_free(buffer);
        board->recording_writer_buffer_ = nullptr;

        const esp_err_t error = board->recording_last_error_.load();
        if (error == ESP_OK)
        {
            board->recording_state_.store(RecordingState::stopped);
            board->notifyRecordingEvent(RecordingEventType::state_changed, RecordingState::stopped);
        }
        else
        {
            board->recording_state_.store(RecordingState::error);
            board->notifyRecordingEvent(RecordingEventType::error, RecordingState::error, error);
        }

        board->recording_writer_task_ = nullptr;
        if (board->recording_done_ != nullptr)
            xSemaphoreGive(board->recording_done_);
        vTaskDeleteWithCaps(nullptr);
    }

    esp_err_t Board::startRecording(const char *path)
    {
        RecordingConfig config{};
        config.path = path;
        return startRecording(config);
    }

    esp_err_t Board::startRecording(const RecordingConfig &config)
    {
        RecordingState current = recording_state_.load();
        if ((current == RecordingState::stopped || current == RecordingState::error) &&
            recording_done_ != nullptr && recording_capture_task_ == nullptr &&
            recording_writer_task_ == nullptr)
        {
            if (xSemaphoreTake(recording_done_, 0) != pdTRUE)
                return ESP_ERR_INVALID_STATE;
            vSemaphoreDelete(recording_done_);
            recording_done_ = nullptr;
            current = recording_state_.load();
        }
        if (current == RecordingState::starting || current == RecordingState::recording ||
            current == RecordingState::stopping || recording_capture_task_ != nullptr ||
            recording_writer_task_ != nullptr)
            return ESP_ERR_INVALID_STATE;
        if (!isAudioInitialized() || !audio_capture_enabled_ || audio_rx_ == nullptr)
            return ESP_ERR_INVALID_STATE;
        if (config.path == nullptr || config.path[0] == '\0')
            return ESP_ERR_INVALID_ARG;
        if (config.ring_buffer_bytes < kMinimumRingBytes ||
            config.ring_buffer_bytes == std::numeric_limits<std::size_t>::max() ||
            config.chunk_bytes < kMinimumChunkBytes || config.chunk_bytes > kMaximumChunkBytes ||
            config.chunk_bytes > config.ring_buffer_bytes / 2U || config.read_timeout_ms == 0U)
            return ESP_ERR_INVALID_ARG;

        const std::uint16_t channels = channelCount(audio_config_);
        const std::uint16_t bits = bitsPerSample(audio_config_.word_length);
        const std::size_t frame_bytes = static_cast<std::size_t>(channels) * bits / 8U;
        if (frame_bytes == 0U || config.chunk_bytes % frame_bytes != 0U)
            return ESP_ERR_INVALID_ARG;

#if !CONFIG_SPIRAM_BOOT_INIT || !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
        // Reject old sdkconfig settings before an external stack can assert.
        return ESP_ERR_NOT_SUPPORTED;
#endif

        const std::size_t path_length = std::strlen(config.path);
        if (path_length >= recording_path_.size())
            return ESP_ERR_INVALID_SIZE;

        FILE *file = std::fopen(config.path, "wb");
        if (file == nullptr)
            return ESP_ERR_NOT_FOUND;

        std::array<std::uint8_t, kWavHeaderBytes> placeholder{};
        if (std::fwrite(placeholder.data(), 1, placeholder.size(), file) != placeholder.size())
        {
            std::fclose(file);
            return ESP_FAIL;
        }

        StreamBufferHandle_t stream = createRecordingStream(config.ring_buffer_bytes);
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        auto *capture_buffer =
            static_cast<std::uint8_t *>(heap_caps_malloc(config.chunk_bytes, kRecordingMemoryCaps));
        auto *writer_buffer =
            static_cast<std::uint8_t *>(heap_caps_malloc(config.chunk_bytes, kRecordingMemoryCaps));
        if (stream == nullptr || done == nullptr || capture_buffer == nullptr ||
            writer_buffer == nullptr)
        {
            if (stream != nullptr)
                deleteRecordingStream(stream);
            if (done != nullptr)
                vSemaphoreDelete(done);
            heap_caps_free(capture_buffer);
            heap_caps_free(writer_buffer);
            std::fclose(file);
            return ESP_ERR_NO_MEM;
        }

        std::memcpy(recording_path_.data(), config.path, path_length + 1U);
        recording_file_ = file;
        recording_stream_buffer_ = stream;
        recording_done_ = done;
        recording_capture_buffer_ = capture_buffer;
        recording_writer_buffer_ = writer_buffer;
        recording_chunk_bytes_ = config.chunk_bytes;
        recording_read_timeout_ms_ = config.read_timeout_ms;
        recording_sample_rate_ = audio_config_.sample_rate_hz;
        recording_channels_ = channels;
        recording_bits_per_sample_ = bits;
        recording_bytes_.store(0U);
        recording_last_error_.store(ESP_OK);
        recording_stop_requested_.store(false);
        recording_capture_done_.store(false);
        recording_started_us_.store(esp_timer_get_time());
        recording_state_.store(RecordingState::starting);
        notifyRecordingEvent(RecordingEventType::state_changed, RecordingState::starting);

        if (xTaskCreateWithCaps(recordingWriterTaskEntry, "p4_rec_write", 8192, this, 8,
                                &recording_writer_task_, kRecordingMemoryCaps) != pdPASS)
        {
            recording_writer_task_ = nullptr;
            recording_stream_buffer_ = nullptr;
            recording_done_ = nullptr;
            deleteRecordingStream(stream);
            vSemaphoreDelete(done);
            heap_caps_free(capture_buffer);
            heap_caps_free(writer_buffer);
            recording_capture_buffer_ = nullptr;
            recording_writer_buffer_ = nullptr;
            recording_file_ = nullptr;
            std::fclose(file);
            recording_state_.store(RecordingState::idle);
            return ESP_ERR_NO_MEM;
        }

        if (xTaskCreateWithCaps(recordingCaptureTaskEntry, "p4_rec_read", 8192, this, 9,
                                &recording_capture_task_, kRecordingMemoryCaps) != pdPASS)
        {
            heap_caps_free(capture_buffer);
            recording_capture_buffer_ = nullptr;
            recording_last_error_.store(ESP_ERR_NO_MEM);
            recording_stop_requested_.store(true);
            recording_capture_done_.store(true);
            // The writer owns the stream and file. Retain its completion signal
            // on timeout so stopRecording() or a later start can safely reap it.
            if (xSemaphoreTake(done, pdMS_TO_TICKS(2000)) == pdTRUE)
            {
                vSemaphoreDelete(done);
                recording_done_ = nullptr;
            }
            return ESP_ERR_NO_MEM;
        }

        return ESP_OK;
    }

    esp_err_t Board::stopRecording(std::uint32_t timeout_ms)
    {
        const RecordingState current = recording_state_.load();
        if (current == RecordingState::idle || current == RecordingState::stopped)
            return ESP_OK;
        if (current == RecordingState::error && recording_done_ == nullptr)
            return recording_last_error_.load();
        if (recording_done_ == nullptr)
            return ESP_ERR_INVALID_STATE;

        recording_state_.store(RecordingState::stopping);
        recording_stop_requested_.store(true);
        notifyRecordingEvent(RecordingEventType::state_changed, RecordingState::stopping);
        if (xSemaphoreTake(recording_done_, timeoutTicks(timeout_ms)) != pdTRUE)
            return ESP_ERR_TIMEOUT;

        const esp_err_t error = recording_last_error_.load();
        vSemaphoreDelete(recording_done_);
        recording_done_ = nullptr;
        recording_capture_task_ = nullptr;
        recording_writer_task_ = nullptr;
        recording_stream_buffer_ = nullptr;
        return error;
    }

    esp_err_t Board::setRecordingEventCallback(RecordingEventCallback callback, void *context)
    {
        recording_callback_context_.store(context);
        recording_callback_.store(callback);
        return ESP_OK;
    }

    RecordingState Board::recordingState() const noexcept
    {
        return recording_state_.load();
    }

    esp_err_t Board::recordingLastError() const noexcept
    {
        return recording_last_error_.load();
    }

    std::uint32_t Board::recordingBytes() const noexcept
    {
        return recording_bytes_.load();
    }

    std::uint32_t Board::recordingDurationMs() const noexcept
    {
        const std::int64_t started_us = recording_started_us_.load();
        if (started_us <= 0)
            return 0U;
        const std::int64_t elapsed_us = esp_timer_get_time() - started_us;
        return elapsed_us <= 0 ? 0U : static_cast<std::uint32_t>(elapsed_us / 1000LL);
    }

    bool Board::isRecording() const noexcept
    {
        const RecordingState state = recording_state_.load();
        return state == RecordingState::starting || state == RecordingState::recording ||
               state == RecordingState::stopping;
    }
} // namespace bsp::T_PANEL_P4_BSP_NAMESPACE
