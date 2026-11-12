#include "record_audio.h"
#include "service_context.hpp"

#include <bsp/board.hpp>
#include <cmath>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

namespace
{
#define s_board (factory::services::board())
    constexpr std::size_t kFrames = 256U;
    constexpr float kDbFloor = -60.0F;
    constexpr std::size_t kChunkBytes = 2048U;
    constexpr std::size_t kRecordCacheBytes = 2U * 1024U * 1024U;
    constexpr float kPlaybackTargetRms = 8192.0F; // About -12 dBFS before output volume.
    constexpr float kPlaybackMaxGain = 32.0F;     // Bound noise amplification to +30 dB.
    constexpr float kPlaybackLimit = 26000.0F;
    constexpr std::size_t kNoiseBins = 128U;
    constexpr float kNoiseBinWidth = 8.0F;
    constexpr float kGateFloor = 0.02F;
    constexpr char kTag[] = "round_recorder";
    std::uint32_t s_peak{};
    std::atomic<bool> s_record_stop{false};
    std::atomic<bool> s_recording{false};
    std::atomic<bool> s_record_paused{false};
    std::uint8_t *s_record_cache{};
    std::atomic<std::size_t> s_record_bytes{0};
    std::atomic<bool> s_record_playing{false};
    std::atomic<bool> s_play_stop{false};
    std::atomic<std::uint8_t> s_record_level{0};
    volatile std::uint8_t s_record_waveform[64]{};
    char s_latest_path[192]{};
    unsigned s_generation{};
    std::uint32_t s_checksum{};
    std::uint32_t s_cache_peak{};
    std::uint64_t s_cache_energy[2]{};
    std::uint32_t s_noise_histogram[kNoiseBins]{};
    std::uint32_t s_noise_blocks{};

    float recordingNoiseFloor()
    {
        // The quietest fifth of recorded blocks estimates the background floor;
        // unlike a fixed threshold, this follows microphone gain and room noise.
        const std::uint32_t rank = std::max<std::uint32_t>(1U, (s_noise_blocks + 4U) / 5U);
        std::uint32_t count = 0U;
        for (std::size_t bin = 0; bin < kNoiseBins; ++bin)
        {
            count += s_noise_histogram[bin];
            if (count >= rank)
                return std::clamp((bin + 0.5F) * kNoiseBinWidth, 8.0F, 128.0F);
        }
        return 8.0F;
    }

    float blockRms(const std::uint64_t (&energy)[2], std::size_t frames)
    {
        return static_cast<float>(std::sqrt(static_cast<double>(std::max(energy[0], energy[1])) /
                                             static_cast<double>(std::max<std::size_t>(1U, frames))));
    }

    float limitPlaybackSample(float sample)
    {
        const float magnitude = std::fabs(sample);
        if (magnitude <= kPlaybackLimit)
            return sample;
        // Soft knee: preserve quiet speech and compress peaks without int16 wrap.
        constexpr float headroom = 32767.0F - kPlaybackLimit;
        const float excess = magnitude - kPlaybackLimit;
        return std::copysign(kPlaybackLimit + headroom * excess / (headroom + excess), sample);
    }

    // Flush more than one complete TX DMA ring before unmuting or ending playback.
    bool flush_output()
    {
        return s_board->drainAudio() == ESP_OK;
    }

    struct SpeechFilter
    {
        float previous_input[2]{};
        float high_pass[2]{};
        float low_pass[2]{};

        void apply(std::int16_t *samples, std::size_t count)
        {
            // 80 Hz high-pass removes DC/rumble; 8 kHz low-pass reduces hiss.
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::size_t channel = i % 2U;
                const float input = samples[i];
                high_pass[channel] = 0.9896F * (high_pass[channel] + input - previous_input[channel]);
                previous_input[channel] = input;
                low_pass[channel] += 0.5115F * (high_pass[channel] - low_pass[channel]);
                samples[i] = static_cast<std::int16_t>(std::clamp(low_pass[channel], -32768.0F, 32767.0F));
            }
        }
    };

    void update_waveform(const std::uint8_t *buffer, std::size_t bytes_read)
    {
        const std::size_t sample_count = bytes_read / sizeof(std::int16_t);
        const auto *samples = reinterpret_cast<const std::int16_t *>(buffer);
        std::int32_t peak = 0;
        for (std::size_t index = 0U; index < sample_count; ++index)
        {
            const std::int32_t value = samples[index] < 0 ? -samples[index] : samples[index];
            if (value > peak)
                peak = value;
        }
        s_peak = static_cast<std::uint32_t>(peak);
        // Linear full-scale percentages truncate quiet microphone input to zero.
        // Map real PCM peaks from -60 to -6 dBFS onto the visible envelope.
        const float db = peak > 0 ? 20.0F * std::log10(static_cast<float>(peak) / 32768.0F) : -60.0F;
        s_record_level = db <= -60.0F ? 0U : db >= -6.0F ? 100U
            : static_cast<std::uint8_t>((db + 60.0F) * 100.0F / 54.0F);
        for (std::size_t index = 0U; index < 63U; ++index)
            s_record_waveform[index] = s_record_waveform[index + 1U];
        s_record_waveform[63] = s_record_level;
    }

    void record_task_entry(void *)
    {
        std::int16_t samples[kChunkBytes / sizeof(std::int16_t)];
        auto *buffer = reinterpret_cast<std::uint8_t *>(samples);
        SpeechFilter filter{};
        const auto config = s_board->audioConfig();
        // RX can contain queued samples from the previous session even after disable/enable.
        std::size_t discard_bytes = static_cast<std::size_t>(config.dma_descriptor_count + 2) *
                                    config.dma_frames_per_descriptor * 4U;
        std::size_t next_log_bytes = 192000U;
        std::uint32_t interval_peak = 0U;
        std::uint32_t raw_left_peak = 0U;
        std::uint32_t raw_right_peak = 0U;
        std::uint32_t clipped_left = 0U;
        std::uint32_t clipped_right = 0U;
        std::uint32_t raw_frames = 0U;
        std::uint32_t checksum = 2166136261U;
        while (!s_record_stop)
        {
            std::size_t bytes_read = 0U;
            const esp_err_t result = s_board->readAudio(samples, sizeof(samples), bytes_read, 100U);
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT)
            {
                ESP_LOGE(kTag, "Capture failed: %s", esp_err_to_name(result));
                break;
            }
            bytes_read -= bytes_read % 4U;
            if (bytes_read == 0U)
                continue;
            if (discard_bytes != 0U)
            {
                discard_bytes -= std::min(discard_bytes, bytes_read);
                continue;
            }
            if (s_record_paused || s_record_stop)
                continue;
            const std::size_t offset = s_record_bytes.load();
            bytes_read = std::min(bytes_read, kRecordCacheBytes - offset);
            for (std::size_t i = 0; i < bytes_read / sizeof(std::int16_t); i += 2U)
            {
                const std::int32_t left = samples[i];
                const std::int32_t right = samples[i + 1U];
                raw_left_peak = std::max(raw_left_peak, static_cast<std::uint32_t>(left < 0 ? -left : left));
                raw_right_peak = std::max(raw_right_peak, static_cast<std::uint32_t>(right < 0 ? -right : right));
                if (left <= -32760 || left >= 32760)
                    ++clipped_left;
                if (right <= -32760 || right >= 32760)
                    ++clipped_right;
                ++raw_frames;
            }
            filter.apply(samples, bytes_read / sizeof(std::int16_t));
            // Measure the filtered PCM that will actually be replayed. Keep both
            // channels separate so an unused/quiet microphone cannot double gain.
            std::uint64_t block_energy[2]{};
            for (std::size_t i = 0; i < bytes_read / sizeof(std::int16_t); ++i)
            {
                const std::int64_t sample = samples[i];
                block_energy[i % 2U] += static_cast<std::uint64_t>(sample * sample);
            }
            s_cache_energy[0] += block_energy[0];
            s_cache_energy[1] += block_energy[1];
            const std::size_t noise_bin = std::min(kNoiseBins - 1U, static_cast<std::size_t>(
                blockRms(block_energy, bytes_read / 4U) / kNoiseBinWidth));
            ++s_noise_histogram[noise_bin];
            ++s_noise_blocks;
            std::memcpy(s_record_cache + offset, samples, bytes_read);
            for (std::size_t i = 0; i < bytes_read; ++i)
                checksum = (checksum ^ buffer[i]) * 16777619U;
            s_record_bytes.store(offset + bytes_read);
            update_waveform(buffer, bytes_read);
            s_cache_peak = std::max(s_cache_peak, s_peak);
            interval_peak = std::max(interval_peak, s_peak);
            if (s_record_bytes >= next_log_bytes)
            {
                ESP_LOGI(kTag, "Recording #%u: bytes=%u raw_L=%lu raw_R=%lu peak=%lu waveform=%u%%", s_generation,
                         static_cast<unsigned>(s_record_bytes.load()),
                         static_cast<unsigned long>(raw_left_peak),
                         static_cast<unsigned long>(raw_right_peak),
                         static_cast<unsigned long>(interval_peak),
                         static_cast<unsigned>(s_record_level.load()));
                if (raw_left_peak == 0U && raw_right_peak == 0U)
                    ESP_LOGW(kTag, "I2S input is all zero; check codec ADC mute and microphone input");
                if (clipped_left != 0U || clipped_right != 0U)
                    ESP_LOGW(kTag, "ADC clipping: L=%lu R=%lu / %lu frames",
                             static_cast<unsigned long>(clipped_left),
                             static_cast<unsigned long>(clipped_right),
                             static_cast<unsigned long>(raw_frames));
                next_log_bytes += 192000U;
                interval_peak = 0U;
                raw_left_peak = 0U;
                raw_right_peak = 0U;
                clipped_left = 0U;
                clipped_right = 0U;
                raw_frames = 0U;
            }
            if (s_record_bytes == kRecordCacheBytes)
                break;
        }
        (void)record_audio_disable_capture();
        s_checksum = checksum;
        ESP_LOGI(kTag, "Recorded #%u: bytes=%u checksum=%08lx", s_generation,
                 static_cast<unsigned>(s_record_bytes.load()), static_cast<unsigned long>(checksum));
        s_record_paused = false;
        // Publish completion only after all cache writes and hardware cleanup finish.
        s_recording.store(false);
        vTaskDelete(nullptr);
    }

    void playback_task_entry(void *argument)
    {
        auto *samples = static_cast<std::int16_t *>(argument);
        auto *buffer = reinterpret_cast<std::uint8_t *>(samples);
        const std::size_t total = s_record_bytes.load();
        const std::size_t frame_count = total / 4U;
        const float rms = static_cast<float>(std::sqrt(
            static_cast<double>(std::max(s_cache_energy[0], s_cache_energy[1])) /
            static_cast<double>(std::max<std::size_t>(1U, frame_count))));
        // A single handling click must not set the gain for the whole recording.
        // Keep one gain for both channels and the entire clip to avoid pumping.
        const float gain = std::clamp(kPlaybackTargetRms / std::max(1.0F, rms), 1.0F, kPlaybackMaxGain);
        const float noise_floor = recordingNoiseFloor();
        const float gate_close = noise_floor * 1.25F;
        const float gate_open = noise_floor * 2.5F;
        // One envelope for both channels avoids shifting stereo balance. Smooth
        // transitions preserve consonants and avoid clicks at the noise boundary.
        float gate = kGateFloor;
        const float sample_rate = static_cast<float>(s_board->audioConfig().sample_rate_hz);
        const float gate_attack = 1.0F - std::exp(-1.0F / (0.003F * sample_rate));
        const float gate_release = 1.0F - std::exp(-1.0F / (0.080F * sample_rate));
        std::size_t offset = 0U;
        ESP_LOGI(kTag, "Playing #%u: bytes=%u checksum=%08lx peak=%lu rms=%.1f gain=%.2fx noise=%.1f gate=%.1f/%.1f volume=%.1f dB [recorder-v4]", s_generation,
                 static_cast<unsigned>(total), static_cast<unsigned long>(s_checksum),
                 static_cast<unsigned long>(s_cache_peak), static_cast<double>(rms),
                 static_cast<double>(gain), static_cast<double>(noise_floor),
                 static_cast<double>(gate_close), static_cast<double>(gate_open),
                 static_cast<double>(s_board->audioVolumeDb()));
        while (!s_play_stop && offset < total)
        {
            const std::size_t chunk = std::min(total - offset, kChunkBytes);
            std::memcpy(samples, s_record_cache + offset, chunk);
            const std::size_t frames = chunk / 4U;
            std::uint64_t block_energy[2]{};
            for (std::size_t i = 0; i < frames * 2U; ++i)
            {
                const std::int64_t sample = samples[i];
                block_energy[i % 2U] += static_cast<std::uint64_t>(sample * sample);
            }
            const float activity = std::clamp((blockRms(block_energy, frames) - gate_close) /
                                                  (gate_open - gate_close), 0.0F, 1.0F);
            const float gate_target = kGateFloor + (1.0F - kGateFloor) * activity * activity;
            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                gate += (gate_target - gate) * (gate_target > gate ? gate_attack : gate_release);
                const std::size_t position = offset / 4U + frame;
                const float fade = std::min(1.0F, static_cast<float>(std::min(position, total / 4U - 1U - position)) / 480.0F);
                for (unsigned channel = 0; channel < 2U; ++channel)
                {
                    const std::size_t index = frame * 2U + channel;
                    samples[index] = static_cast<std::int16_t>(limitPlaybackSample(samples[index] * gain * gate) * fade);
                }
            }
            std::size_t written = 0U;
            const esp_err_t result = s_board->writeAudio(samples, chunk, written, 100U);
            offset += written;
            if (written != 0U)
                update_waveform(buffer, written);
            if (result != ESP_OK && result != ESP_ERR_TIMEOUT)
            {
                ESP_LOGE(kTag, "Playback failed: %s", esp_err_to_name(result));
                break;
            }
            if (written == 0U)
                break;
        }
        if (s_play_stop || offset < total)
            (void)s_board->setSpeakerEnabled(false);
        // Drain the last audio and replace queued DMA data with silence.
        if (!flush_output())
            ESP_LOGW(kTag, "Playback TX drain failed");
        if (s_board->setSpeakerEnabled(false) != ESP_OK)
            ESP_LOGE(kTag, "Failed to disable playback PA");
        ESP_LOGI(kTag, "Playback complete [recorder-v4], stack free minimum=%u bytes",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        heap_caps_free(samples);
        s_record_playing.store(false);
        vTaskDelete(nullptr);
    }
}

extern "C" bool record_audio_read_db(float *left_db, float *right_db)
{
    if (left_db == nullptr || right_db == nullptr || s_board == nullptr || s_recording)
        return false;

    std::int16_t samples[kFrames * 2U]{};
    std::size_t bytes_read = 0U;
    if (s_board->readAudio(samples, sizeof(samples), bytes_read, 20U) != ESP_OK ||
        bytes_read < sizeof(samples))
        return false;

    double left_sum = 0.0;
    double right_sum = 0.0;
    for (std::size_t frame = 0U; frame < kFrames; ++frame)
    {
        const double left = static_cast<double>(samples[frame * 2U]);
        const double right = static_cast<double>(samples[frame * 2U + 1U]);
        left_sum += left * left;
        right_sum += right * right;
    }

    const auto to_db = [](double sum) {
        const double rms = std::sqrt(sum / static_cast<double>(kFrames));
        if (rms <= 1.0)
            return kDbFloor;
        const float db = 20.0F * std::log10(static_cast<float>(rms / 32768.0));
        return db < kDbFloor ? kDbFloor : db;
    };
    *left_db = to_db(left_sum);
    *right_db = to_db(right_sum);
    return true;
}

extern "C" bool record_audio_enable_capture(void)
{
    if (s_board == nullptr || !s_board->isAudioInitialized())
        return false;
    if (s_board->isAudioCaptureEnabled())
        return true;
    return s_board->setAudioCaptureEnabled(true) == ESP_OK;
}

extern "C" bool record_audio_disable_capture(void)
{
    if (s_board == nullptr || !s_board->isAudioInitialized())
        return true;
    if (!s_board->isAudioCaptureEnabled())
        return true;
    return s_board->setAudioCaptureEnabled(false) == ESP_OK;
}

extern "C" bool record_audio_is_recording(void) { return s_recording; }
extern "C" bool record_audio_is_paused(void) { return s_record_paused; }
extern "C" bool record_audio_is_playing(void) { return s_record_playing; }
extern "C" uint32_t record_audio_duration_ms(void)
{
    // 48 kHz, stereo, 16-bit PCM. Count only samples actually retained.
    return static_cast<uint32_t>(static_cast<uint64_t>(s_record_bytes.load()) * 1000U / 192000U);
}
extern "C" size_t record_audio_cache_bytes(void) { return s_record_bytes; }
extern "C" uint8_t record_audio_level_percent(void) { return s_record_level; }
extern "C" void record_audio_get_waveform(uint8_t *levels, size_t count)
{
    if (levels == nullptr || count == 0U)
        return;
    const size_t copy_count = count < 64U ? count : 64U;
    for (size_t index = 0U; index < copy_count; ++index)
        levels[index] = s_record_waveform[index];
    for (size_t index = copy_count; index < count; ++index)
        levels[index] = 0U;
}

extern "C" bool record_audio_start(void)
{
    if (s_board == nullptr || s_recording)
    {
        ESP_LOGE(kTag, "Cannot start recording: board=%p, recording=%d", s_board, s_recording.load());
        return false;
    }
    if (s_record_playing)
    {
        ESP_LOGE(kTag, "Cannot start recording while cached playback is active");
        return false;
    }
    // Keep the speaker off while explicitly clearing ADC hardware/software
    // mute. The BSP separates DAC mute (R40) from ADC mute (R20).
    const auto check = [](const char *step, esp_err_t result) {
        if (result != ESP_OK)
            ESP_LOGE(kTag, "Capture startup failed at %s: %s", step, esp_err_to_name(result));
        return result == ESP_OK;
    };
    if (!check("stop music", s_board->stopMusic()) ||
        !check("disable output", s_board->setAudioOutputEnabled(false)) ||
        !check("disable capture", record_audio_disable_capture() ? ESP_OK : ESP_FAIL) ||
        !check("enable capture", record_audio_enable_capture() ? ESP_OK : ESP_FAIL) ||
        !check("unmute ADC", s_board->setAudioCaptureMuted(false)) ||
        // Respect the microphone gain selected by the application.
        !check("microphone gain", s_board->setMicrophoneGain(s_board->audioConfig().microphone_gain,
                                 bsp::t_panel_p4_round::AudioMicrophoneChannel::both)))
    {
        ESP_LOGE(kTag, "Microphone capture unavailable");
        return false;
    }
    if (s_record_cache == nullptr)
        s_record_cache = static_cast<std::uint8_t *>(
            heap_caps_malloc(kRecordCacheBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_record_cache == nullptr)
    {
        (void)record_audio_disable_capture();
        ESP_LOGE(kTag, "Unable to allocate 2 MiB recording buffer in PSRAM");
        return false;
    }
    ++s_generation;
    s_checksum = 0U;
    s_cache_peak = 0U;
    s_cache_energy[0] = 0U;
    s_cache_energy[1] = 0U;
    std::memset(s_noise_histogram, 0, sizeof(s_noise_histogram));
    s_noise_blocks = 0U;
    s_record_bytes = 0U;
    s_record_level = 0U;
    for (std::size_t index = 0U; index < 64U; ++index)
        s_record_waveform[index] = 0U;
    s_record_stop = false;
    s_record_paused = false;
    s_recording = true;
    if (xTaskCreate(record_task_entry, "record_ui", 8192, nullptr, 8, nullptr) != pdPASS)
    {
        s_recording = false;
        (void)record_audio_disable_capture();
        ESP_LOGE(kTag, "Unable to create microphone capture task");
        return false;
    }
    ESP_LOGI(kTag, "Microphone capture started #%u", s_generation);
    return true;
}

extern "C" bool record_audio_pause(bool paused)
{
    if (!s_recording)
        return false;
    if (paused == s_record_paused)
        return true;
    s_record_paused = paused;
    return true;
}

// This example retains PCM in PSRAM only. SD-card writes are disabled.
extern "C" bool record_audio_save(void) { return false; }

extern "C" bool record_audio_play_cached(void)
{
    if (s_board == nullptr || !s_board->isAudioInitialized() ||
        s_recording || s_record_playing ||
        s_record_cache == nullptr || s_record_bytes == 0U)
        return false;
    if (s_board->stopMusic() != ESP_OK || s_board->setAudioOutputEnabled(false) != ESP_OK ||
        !record_audio_disable_capture() || !flush_output())
        return false;
    // Recorded-audio playback uses the same public output volume as music and
    // the settings panel. Do not temporarily force 0 dB and overwrite the
    // user's shared volume setting.
    if (s_board->setAudioMuted(false) != ESP_OK ||
        s_board->setAudioOutputEnabled(true) != ESP_OK)
    {
        (void)s_board->setAudioOutputEnabled(false);
        return false;
    }
    s_play_stop = false;
    auto *samples = static_cast<std::int16_t *>(heap_caps_malloc(kChunkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (samples == nullptr)
    {
        (void)s_board->setAudioOutputEnabled(false);
        return false;
    }
    s_record_level = 0U;
    for (std::size_t index = 0U; index < 64U; ++index)
        s_record_waveform[index] = 0U;
    s_record_playing = true;
    if (xTaskCreate(playback_task_entry, "record_play", 12288, samples, 7, nullptr) != pdPASS)
    {
        heap_caps_free(samples);
        s_record_playing = false;
        (void)s_board->setAudioOutputEnabled(false);
        return false;
    }
    return true;
}

extern "C" bool record_audio_stop_cached_playback(void)
{
    if (!s_record_playing)
        return true;
    s_play_stop = true;
    const TickType_t start = xTaskGetTickCount();
    while (s_record_playing && xTaskGetTickCount() - start < pdMS_TO_TICKS(2000U))
        vTaskDelay(pdMS_TO_TICKS(10U));
    return !s_record_playing;
}

extern "C" void record_audio_clear_cache(void)
{
    if (!record_audio_stop() || !record_audio_stop_cached_playback())
        return;
    if (s_record_cache != nullptr)
        heap_caps_free(s_record_cache);
    s_record_cache = nullptr;
    s_record_bytes = 0U;
    s_record_level = 0U;
    std::memset((void *)s_record_waveform, 0, sizeof(s_record_waveform));
}

extern "C" bool record_audio_stop(void)
{
    if (!s_recording)
        return true;
    if (s_record_paused)
        (void)record_audio_pause(false);
    s_record_stop = true;
    const TickType_t start = xTaskGetTickCount();
    while (s_recording && xTaskGetTickCount() - start < pdMS_TO_TICKS(2000U))
        vTaskDelay(pdMS_TO_TICKS(10U));
    return !s_recording;
}

extern "C" bool record_audio_latest_path(char *path, size_t size)
{
    if (path == nullptr || size == 0U)
        return false;
    if (s_latest_path[0] == '\0')
        return false;
    std::snprintf(path, size, "%s", s_latest_path);
    return true;
}

extern "C" bool record_audio_wav_duration(const char *path, uint32_t *duration_ms)
{
    if (duration_ms != nullptr)
        *duration_ms = 0U;
    if (path == nullptr || duration_ms == nullptr)
        return false;
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    std::uint8_t header[44]{};
    const bool ok = std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
                    std::memcmp(header, "RIFF", 4) == 0 &&
                    std::memcmp(header + 8, "WAVE", 4) == 0;
    std::fclose(file);
    if (!ok)
        return false;
    const std::uint32_t byte_rate = static_cast<std::uint32_t>(header[28]) |
                                    (static_cast<std::uint32_t>(header[29]) << 8U) |
                                    (static_cast<std::uint32_t>(header[30]) << 16U) |
                                    (static_cast<std::uint32_t>(header[31]) << 24U);
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(header[40]) |
                                     (static_cast<std::uint32_t>(header[41]) << 8U) |
                                     (static_cast<std::uint32_t>(header[42]) << 16U) |
                                     (static_cast<std::uint32_t>(header[43]) << 24U);
    if (byte_rate == 0U)
        return false;
    *duration_ms = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(data_bytes) * 1000ULL) / byte_rate);
    return true;
}
