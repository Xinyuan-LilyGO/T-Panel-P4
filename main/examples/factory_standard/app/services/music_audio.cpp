#include "music_audio.h"
#include "service_context.hpp"
#include <bsp/t_panel_p4_standard.hpp>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#define s_board (factory::services::board())
static const char *s_last_error_name = "ESP_OK";
static constexpr char kTag[] = "factory_music";
static bool s_music_heap_logged{};

static void log_music_heap_snapshot()
{
    struct HeapType
    {
        const char *name;
        std::uint32_t capabilities;
    };
    constexpr HeapType heap_types[] = {
        {"INTERNAL", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
        {"DMA", MALLOC_CAP_DMA | MALLOC_CAP_8BIT},
        {"SPIRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT},
    };

    ESP_LOGI(kTag, "heap snapshot: after music player initialization");
    for (const HeapType &heap : heap_types)
    {
        ESP_LOGI(kTag, "  %-8s free=%u, largest=%u, minimum=%u",
                 heap.name,
                 heap_caps_get_free_size(heap.capabilities),
                 heap_caps_get_largest_free_block(heap.capabilities),
                 heap_caps_get_minimum_free_size(heap.capabilities));
    }
}

static const char *music_codec_name(bsp::t_panel_p4_standard::MusicCodec codec)
{
    using bsp::t_panel_p4_standard::MusicCodec;
    switch (codec)
    {
    case MusicCodec::mp3: return "MP3";
    case MusicCodec::wav: return "WAV";
    case MusicCodec::aac: return "AAC";
    case MusicCodec::flac: return "FLAC";
    case MusicCodec::m4a: return "M4A";
    case MusicCodec::opus: return "OPUS";
    case MusicCodec::ts: return "TS";
    default: return "UNKNOWN";
    }
}

static void music_event_callback(
    const bsp::t_panel_p4_standard::MusicEvent &event, void *)
{
    if (event.type == bsp::t_panel_p4_standard::MusicEventType::music_info)
    {
        ESP_LOGI(kTag, "format=%ld Hz, %u ch, %u bit, duration=%lu ms",
                 static_cast<long>(event.format.sample_rate_hz),
                 static_cast<unsigned>(event.format.channels),
                 static_cast<unsigned>(event.format.bits_per_sample),
                 static_cast<unsigned long>(event.duration_ms));
        if (!s_music_heap_logged)
        {
            s_music_heap_logged = true;
            log_music_heap_snapshot();
        }
    }
    else if (event.type == bsp::t_panel_p4_standard::MusicEventType::error)
    {
        ESP_LOGE(kTag, "playback error: %s", esp_err_to_name(event.error));
    }
}
extern "C" bool music_audio_enter(void)
{
    if (!s_board)
    {
        s_last_error_name = "ESP_ERR_INVALID_STATE";
        return false;
    }
    const esp_err_t result = s_board->setMusicEventCallback(music_event_callback);
    s_last_error_name = esp_err_to_name(result);
    /* Playback is queued to the dedicated music task. A timeout means the
     * command is still pending, so do not make the UI wait for the decoder. */
    return result == ESP_OK || result == ESP_ERR_TIMEOUT;
}
extern "C" bool music_audio_leave(void)
{
    if (!s_board)
    {
        s_last_error_name = "ESP_ERR_INVALID_STATE";
        return false;
    }
    const esp_err_t result = s_board->deinitializeMusic();
    s_last_error_name = esp_err_to_name(result);
    if (result != ESP_OK)
        ESP_LOGE(kTag, "music player deinitialize failed: %s", s_last_error_name);
    return result == ESP_OK;
}
extern "C" bool music_audio_play(const char *path)
{
    if (!s_board)
    {
        s_last_error_name = "ESP_ERR_INVALID_STATE";
        return false;
    }
    bsp::t_panel_p4_standard::MusicPlaybackConfig playback_config{};
    // Keep the ES8389/I2S clock fixed and convert each track to the output
    // rate. Reconfiguring the codec while switching tracks is not reliable.
    playback_config.auto_configure_audio = false;
    playback_config.allow_resample = true;
    // 设置拒绝未知格式
    playback_config.reject_unknown_format = true;
    s_board->resetMusicStatistics();
    const esp_err_t result = s_board->playMusicFile(path, playback_config);
    s_last_error_name = esp_err_to_name(result);
    return result == ESP_OK;
}
extern "C" const char *music_audio_last_error_name(void) { return s_last_error_name; }
extern "C" void music_audio_pause(bool paused)
{
    if (s_board)
        (void)s_board->pauseMusic(paused);
}
extern "C" void music_audio_stop(void)
{
    if (s_board)
        (void)s_board->stopMusic();
}
extern "C" bool music_audio_is_playing(void) { return s_board && s_board->isMusicPlaying(); }
extern "C" bool music_audio_is_paused(void) { return s_board && s_board->isMusicPaused(); }
extern "C" music_audio_state_t music_audio_state(void)
{
    if (!s_board)
        return MUSIC_AUDIO_STATE_IDLE;
    return static_cast<music_audio_state_t>(s_board->musicState());
}
extern "C" void music_audio_get_format(music_audio_format_t *format)
{
    if (!format)
        return;
    *format = {};
    format->codec = "UNKNOWN";
    if (!s_board)
        return;
    const bsp::t_panel_p4_standard::MusicFormat bsp_format = s_board->musicFormat();
    format->codec = music_codec_name(bsp_format.codec);
    format->sample_rate_hz = bsp_format.sample_rate_hz;
    format->bitrate_bps = bsp_format.bitrate_bps;
    format->channels = bsp_format.channels;
    format->bits_per_sample = bsp_format.bits_per_sample;
}
extern "C" uint32_t music_audio_position_ms(void) { return s_board ? s_board->musicPositionMs() : 0; }
extern "C" uint32_t music_audio_duration_ms(void) { return s_board ? s_board->musicDurationMs() : 0; }
extern "C" uint8_t music_audio_level_percent(void) { return s_board ? s_board->musicLevelPercent() : 0; }
extern "C" bool music_audio_current_path(char *path, size_t size)
{
    if (path == nullptr || size == 0U)
        return false;
    path[0] = '\0';
    if (s_board == nullptr)
        return false;
    s_board->getCurrentMusicPath(path, size);
    return path[0] != '\0';
}
extern "C" void music_audio_get_waveform(int16_t *samples, size_t count)
{
    if (s_board != nullptr)
        s_board->getMusicWaveform(samples, count);
    else if (samples != nullptr)
        std::memset(samples, 0, count * sizeof(*samples));
}
extern "C" bool music_audio_probe_duration(const char *path, uint32_t *duration_ms)
{
    if (duration_ms != nullptr)
        *duration_ms = 0U;
    if (s_board == nullptr || path == nullptr || duration_ms == nullptr)
        return false;
    bsp::t_panel_p4_standard::MusicProbe probe{};
    if (s_board->probeMusicFile(path, probe) != ESP_OK)
        return false;
    *duration_ms = probe.duration_ms;
    return probe.duration_ms > 0U;
}
extern "C" bool music_audio_seek_ms(uint32_t position_ms)
{
    if (!s_board)
        return false;
    const esp_err_t result = s_board->seekMusicMs(position_ms);
    if (result != ESP_OK)
        ESP_LOGW(kTag, "music seek failed position=%lu: %s",
                 static_cast<unsigned long>(position_ms), esp_err_to_name(result));
    return result == ESP_OK;
}
extern "C" void music_audio_set_volume_percent(int32_t percent)
{
    if (!s_board)
        return;
    if (percent < 0)
        percent = 0;
    else if (percent > 100)
        percent = 100;
    constexpr float minimum_db = -95.5F;
    constexpr float maximum_db = 32.0F;
    const float db = minimum_db +
                     (maximum_db - minimum_db) * static_cast<float>(percent) / 100.0F;
    (void)s_board->setAudioVolumeDb(db);
}
extern "C" uint8_t music_audio_get_volume_percent(void)
{
    if (!s_board || !s_board->isAudioInitialized())
        return 0U;
    constexpr float minimum_db = -95.5F;
    constexpr float maximum_db = 32.0F;
    const float db = s_board->audioVolumeDb();
    const float ratio = (db - minimum_db) / (maximum_db - minimum_db);
    if (ratio <= 0.0F)
        return 0U;
    if (ratio >= 1.0F)
        return 100U;
    return static_cast<uint8_t>(ratio * 100.0F + 0.5F);
}
extern "C" bool music_audio_load_metadata(const char *path,
                                          uint8_t **cover_data, size_t *cover_size,
                                          char *lyrics, size_t lyrics_size)
{
    if (!s_board || !path)
        return false;
    if (cover_data)
        *cover_data = nullptr;
    if (cover_size)
        *cover_size = 0U;
    std::vector<std::uint8_t> cover;
    std::string text;
    const esp_err_t metadata_result = s_board->getMusicMetadata(path, cover, text);
    const bool has_cover = metadata_result == ESP_OK && !cover.empty();
    const bool has_lyrics = metadata_result == ESP_OK && !text.empty();
    bool cover_copied = false;
    if (has_cover && cover_data && cover_size)
    {
        auto *copy = static_cast<uint8_t *>(heap_caps_malloc(
            cover.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (copy != nullptr)
        {
            std::memcpy(copy, cover.data(), cover.size());
            *cover_data = copy;
            *cover_size = cover.size();
            cover_copied = true;
        }
    }
    if (lyrics && lyrics_size)
    {
        const size_t count = text.size() < lyrics_size - 1 ? text.size() : lyrics_size - 1;
        std::memcpy(lyrics, text.data(), count);
        lyrics[count] = '\0';
    }
    ESP_LOGI(kTag, "metadata: cover=%u (%u bytes), lyrics=%u (%u bytes)",
             cover_copied ? 1U : 0U, static_cast<unsigned>(cover.size()),
             has_lyrics ? 1U : 0U, static_cast<unsigned>(text.size()));
    return cover_copied || has_lyrics;
}

extern "C" void music_audio_free_cover(uint8_t *cover_data)
{
    if (cover_data == nullptr)
        return;
    if (!esp_ptr_internal(cover_data) && !esp_ptr_external_ram(cover_data))
    {
        ESP_LOGW(kTag, "skip invalid cover buffer %p", cover_data);
        return;
    }
    heap_caps_free(cover_data);
}

namespace
{
    std::uint32_t read_be32(const std::uint8_t *data)
    {
        return (static_cast<std::uint32_t>(data[0]) << 24U) |
               (static_cast<std::uint32_t>(data[1]) << 16U) |
               (static_cast<std::uint32_t>(data[2]) << 8U) |
               static_cast<std::uint32_t>(data[3]);
    }

    std::uint32_t read_synchsafe32(const std::uint8_t *data)
    {
        return (static_cast<std::uint32_t>(data[0] & 0x7FU) << 21U) |
               (static_cast<std::uint32_t>(data[1] & 0x7FU) << 14U) |
               (static_cast<std::uint32_t>(data[2] & 0x7FU) << 7U) |
               static_cast<std::uint32_t>(data[3] & 0x7FU);
    }

    void copy_id3_text(const std::uint8_t *data, std::size_t size,
                       char *output, std::size_t output_size)
    {
        if (output == nullptr || output_size == 0U)
            return;
        output[0] = '\0';
        if (data == nullptr || size < 2U)
            return;

        const std::uint8_t encoding = data[0];
        const std::uint8_t *text = data + 1U;
        std::size_t text_size = size - 1U;
        if (encoding == 1U || encoding == 2U)
        {
            if (text_size >= 2U && ((text[0] == 0xFFU && text[1] == 0xFEU) ||
                                    (text[0] == 0xFEU && text[1] == 0xFFU)))
            {
                text += 2U;
                text_size -= 2U;
            }
            std::size_t written = 0U;
            for (std::size_t index = 0U; index + 1U < text_size &&
                                         written + 1U < output_size;
                 index += 2U)
            {
                const std::uint8_t value = encoding == 1U ? text[index] : text[index + 1U];
                if (value == 0U)
                    break;
                output[written++] = static_cast<char>(value);
            }
            output[written] = '\0';
            return;
        }

        std::size_t written = 0U;
        while (written < text_size && written + 1U < output_size && text[written] != 0U)
        {
            output[written] = static_cast<char>(text[written]);
            ++written;
        }
        output[written] = '\0';
    }
}

extern "C" bool music_audio_read_metadata(const char *path,
                                          char *title, size_t title_size,
                                          char *artist, size_t artist_size,
                                          char *album, size_t album_size)
{
    if (title != nullptr && title_size > 0U)
        title[0] = '\0';
    if (artist != nullptr && artist_size > 0U)
        artist[0] = '\0';
    if (album != nullptr && album_size > 0U)
        album[0] = '\0';
    if (path == nullptr)
        return false;

    FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    std::uint8_t header[10]{};
    const bool has_header = std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
                            std::memcmp(header, "ID3", 3) == 0;
    if (!has_header)
    {
        std::fclose(file);
        return false;
    }

    const std::uint8_t version = header[3];
    const std::uint32_t tag_size = read_synchsafe32(header + 6U);
    std::uint32_t offset = 0U;
    bool found = false;
    bool title_done = title == nullptr || title_size == 0U;
    bool artist_done = artist == nullptr || artist_size == 0U;
    bool album_done = album == nullptr || album_size == 0U;
    while (offset + 10U <= tag_size)
    {
        std::uint8_t frame[10]{};
        if (std::fread(frame, 1, sizeof(frame), file) != sizeof(frame))
            break;
        if (frame[0] == 0U && frame[1] == 0U && frame[2] == 0U && frame[3] == 0U)
            break;
        const std::uint32_t frame_size = version >= 4U
                                             ? read_synchsafe32(frame + 4U)
                                             : read_be32(frame + 4U);
        if (frame_size == 0U || frame_size > tag_size - offset - 10U)
            break;
        char *output = nullptr;
        std::size_t output_size = 0U;
        if (std::memcmp(frame, "TIT2", 4) == 0)
        {
            output = title;
            output_size = title_size;
            title_done = true;
        }
        else if (std::memcmp(frame, "TPE1", 4) == 0)
        {
            output = artist;
            output_size = artist_size;
            artist_done = true;
        }
        else if (std::memcmp(frame, "TALB", 4) == 0)
        {
            output = album;
            output_size = album_size;
            album_done = true;
        }
        if (output != nullptr)
        {
            std::vector<std::uint8_t> payload(frame_size);
            if (std::fread(payload.data(), 1, payload.size(), file) != payload.size())
                break;
            copy_id3_text(payload.data(), payload.size(), output, output_size);
            found = output[0] != '\0' || found;
        }
        else if (std::fseek(file, static_cast<long>(frame_size), SEEK_CUR) != 0)
            break;
        offset += 10U + frame_size;
        if (title_done && artist_done && album_done)
            break;
    }
    std::fclose(file);
    return found;
}
