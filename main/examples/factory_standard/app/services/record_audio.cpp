#include "record_audio.h"
#include "service_context.hpp"

#include <bsp/t_panel_p4_standard.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>

namespace
{
#define s_board (factory::services::board())
    constexpr std::size_t kFrames = 256U;
    constexpr float kDbFloor = -60.0F;
    constexpr char kRecordDirectory[] = "/sdcard/record";
    constexpr std::size_t kChunkBytes = 4096U;
    constexpr std::size_t kRecordCacheBytes = 4U * 1024U * 1024U;
    volatile bool s_record_stop{};
    volatile bool s_recording{};
    volatile bool s_record_paused{};
    TaskHandle_t s_record_task{};
    std::uint8_t *s_record_cache{};
    volatile std::size_t s_record_bytes{};
    volatile bool s_record_playing{};
    volatile bool s_play_stop{};
    TaskHandle_t s_play_task{};
    volatile std::uint8_t s_record_level{};
    volatile std::uint8_t s_record_waveform[64]{};
    std::int64_t s_record_started_us{};
    std::int64_t s_record_pause_started_us{};
    std::int64_t s_record_paused_us{};
    char s_latest_path[192]{};

    void put_le16(std::uint8_t *data, std::uint16_t value)
    {
        data[0] = static_cast<std::uint8_t>(value);
        data[1] = static_cast<std::uint8_t>(value >> 8U);
    }

    void put_le32(std::uint8_t *data, std::uint32_t value)
    {
        data[0] = static_cast<std::uint8_t>(value);
        data[1] = static_cast<std::uint8_t>(value >> 8U);
        data[2] = static_cast<std::uint8_t>(value >> 16U);
        data[3] = static_cast<std::uint8_t>(value >> 24U);
    }

    void write_wav_header(FILE *file, std::uint32_t data_bytes)
    {
        std::uint8_t header[44]{};
        std::memcpy(header, "RIFF", 4);
        put_le32(header + 4, 36U + data_bytes);
        std::memcpy(header + 8, "WAVEfmt ", 8);
        put_le32(header + 16, 16U);
        put_le16(header + 20, 1U);
        put_le16(header + 22, 2U);
        put_le32(header + 24, 48000U);
        put_le32(header + 28, 192000U);
        put_le16(header + 32, 4U);
        put_le16(header + 34, 16U);
        std::memcpy(header + 36, "data", 4);
        put_le32(header + 40, data_bytes);
        std::fseek(file, 0, SEEK_SET);
        std::fwrite(header, 1, sizeof(header), file);
        std::fflush(file);
    }

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
        s_record_level = static_cast<std::uint8_t>(peak > 32767 ? 100 : peak * 100 / 32768);
        for (std::size_t index = 0U; index < 63U; ++index)
            s_record_waveform[index] = s_record_waveform[index + 1U];
        s_record_waveform[63] = s_record_level;
    }

    void record_task_entry(void *)
    {
        std::uint8_t buffer[kChunkBytes];
        while (!s_record_stop && s_board != nullptr && s_record_cache != nullptr)
        {
            if (s_record_paused)
            {
                vTaskDelay(pdMS_TO_TICKS(20U));
                continue;
            }
            std::size_t bytes_read = 0U;
            const esp_err_t result = s_board->readAudio(
                buffer, sizeof(buffer), bytes_read, 100U);
            if (result == ESP_ERR_TIMEOUT)
                continue;
            if (result != ESP_OK || bytes_read == 0U)
                break;
            if (s_record_bytes + bytes_read > kRecordCacheBytes)
                break;
            std::memcpy(s_record_cache + s_record_bytes, buffer, bytes_read);
            s_record_bytes += bytes_read;
            update_waveform(buffer, bytes_read);
        }
        s_recording = false;
        s_record_task = nullptr;
        vTaskDelete(nullptr);
    }

    void playback_task_entry(void *)
    {
        std::uint8_t buffer[kChunkBytes];
        std::size_t offset = 0U;
        while (!s_play_stop && s_board != nullptr && s_record_cache != nullptr &&
               offset < s_record_bytes)
        {
            const std::size_t remaining = s_record_bytes - offset;
            const std::size_t chunk = remaining < kChunkBytes ? remaining : kChunkBytes;
            std::memcpy(buffer, s_record_cache + offset, chunk);
            std::size_t written = 0U;
            if (s_board->writeAudio(buffer, chunk, written, 1000U) != ESP_OK ||
                written == 0U)
                break;
            offset += written;
            update_waveform(buffer, written);
        }
        s_record_playing = false;
        s_play_task = nullptr;
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
    if (!s_recording || s_record_started_us <= 0)
        return 0U;
    std::int64_t paused_us = s_record_paused_us;
    if (s_record_paused && s_record_pause_started_us > 0)
        paused_us += esp_timer_get_time() - s_record_pause_started_us;
    const std::int64_t elapsed = esp_timer_get_time() - s_record_started_us - paused_us;
    return elapsed > 0 ? static_cast<uint32_t>(elapsed / 1000LL) : 0U;
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
    if (s_board == nullptr || s_recording || s_record_task != nullptr)
        return false;
    if (s_record_playing)
        return false;
    if (s_record_cache == nullptr)
        s_record_cache = static_cast<std::uint8_t *>(
            heap_caps_malloc(kRecordCacheBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_record_cache == nullptr)
        return false;
    s_record_bytes = 0U;
    s_record_level = 0U;
    for (std::size_t index = 0U; index < 64U; ++index)
        s_record_waveform[index] = 0U;
    s_record_started_us = esp_timer_get_time();
    s_record_pause_started_us = 0;
    s_record_paused_us = 0;
    s_record_stop = false;
    s_record_paused = false;
    s_recording = true;
    if (xTaskCreate(record_task_entry, "record_ui", 8192, nullptr, 8, &s_record_task) != pdPASS)
    {
        s_recording = false;
        return false;
    }
    return true;
}

extern "C" bool record_audio_pause(bool paused)
{
    if (!s_recording)
        return false;
    if (paused == s_record_paused)
        return true;
    const std::int64_t now = esp_timer_get_time();
    if (paused)
        s_record_pause_started_us = now;
    else if (s_record_pause_started_us > 0)
    {
        s_record_paused_us += now - s_record_pause_started_us;
        s_record_pause_started_us = 0;
    }
    s_record_paused = paused;
    return true;
}

extern "C" bool record_audio_save(void)
{
    if (s_recording || s_record_bytes == 0U || s_record_cache == nullptr)
        return false;
    (void)mkdir(kRecordDirectory, 0777);
    char path[sizeof(s_latest_path)]{};
    FILE *file = nullptr;
    for (unsigned index = 0U; index < 1000U && file == nullptr; ++index)
    {
        std::snprintf(path, sizeof(path), "%s/REC_%03u.WAV", kRecordDirectory, index);
        FILE *probe = std::fopen(path, "rb");
        if (probe != nullptr)
        {
            std::fclose(probe);
            continue;
        }
        file = std::fopen(path, "wb");
    }
    if (file == nullptr)
        return false;
    write_wav_header(file, static_cast<std::uint32_t>(s_record_bytes));
    const bool ok = std::fwrite(s_record_cache, 1, s_record_bytes, file) == s_record_bytes;
    std::fflush(file);
    std::fclose(file);
    if (!ok)
    {
        std::remove(path);
        return false;
    }
    std::snprintf(s_latest_path, sizeof(s_latest_path), "%s", path);
    return true;
}

extern "C" bool record_audio_play_cached(void)
{
    if (s_recording || s_record_playing || s_play_task != nullptr ||
        s_record_cache == nullptr || s_record_bytes == 0U)
        return false;
    s_play_stop = false;
    s_record_playing = true;
    if (xTaskCreate(playback_task_entry, "record_play", 8192, nullptr, 7, &s_play_task) != pdPASS)
    {
        s_record_playing = false;
        return false;
    }
    return true;
}

extern "C" bool record_audio_stop_cached_playback(void)
{
    if (!s_record_playing && s_play_task == nullptr)
        return true;
    s_play_stop = true;
    const TickType_t start = xTaskGetTickCount();
    while (s_record_playing && xTaskGetTickCount() - start < pdMS_TO_TICKS(1000U))
        vTaskDelay(pdMS_TO_TICKS(10U));
    return !s_record_playing;
}

extern "C" void record_audio_clear_cache(void)
{
    (void)record_audio_stop();
    (void)record_audio_stop_cached_playback();
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
