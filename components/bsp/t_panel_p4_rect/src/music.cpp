#include <bsp/t_panel_p4_rect.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <esp_audio_simple_player.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace bsp::t_panel_p4_rect
{
constexpr char kTag[] = "t_panel_p4_music";
constexpr std::size_t kPathBytes = 192U;
constexpr std::size_t kMaxId3Bytes = 1024U * 1024U;
constexpr EventBits_t kCommandComplete = BIT0;
constexpr std::uint32_t kCommandTimeoutMs = 5000U;
constexpr std::uint32_t kOutputTimeoutMs = 1000U;
constexpr std::uint32_t kPlaybackStallTimeoutMs = 10000U;

enum class CommandType : std::uint8_t
{
    play,
    pause,
    resume,
    stop,
    shutdown,
};

struct MusicCommand
{
    CommandType type{};
    std::uint32_t sequence{};
    char path[kPathBytes]{};
    MusicProbe probe{};
    bool has_probe{};
};

struct MusicRuntime
{
    Board *board{};
    TaskHandle_t task{};
    QueueHandle_t commands{};
    SemaphoreHandle_t command_mutex{};
    EventGroupHandle_t command_events{};
    esp_asp_handle_t player{};

    std::atomic<bool> task_exited{false};
    std::atomic<bool> playing{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> output_callback_active{false};
    std::atomic<MusicState> state{MusicState::idle};
    std::atomic<MusicFormat> format{};
    std::atomic<std::uint32_t> position_ms{0};
    std::atomic<std::uint32_t> duration_ms{0};
    std::atomic<esp_err_t> last_error{ESP_OK};
    std::atomic<std::uint32_t> next_sequence{0};
    std::atomic<std::uint32_t> completed_sequence{0};
    std::atomic<esp_err_t> completed_result{ESP_OK};
    std::atomic<std::uint32_t> output_calls{0};
    std::atomic<std::uint32_t> output_bytes{0};
    std::atomic<std::uint32_t> output_short_writes{0};
    std::atomic<std::uint32_t> output_errors{0};
    std::atomic<std::uint32_t> playback_timeouts{0};
    std::atomic<std::uint32_t> command_timeouts{0};
    std::atomic<std::uint32_t> stack_high_watermark_words{0};

    char path[kPathBytes]{};
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    MusicEventCallback callback{};
    void *callback_context{};
    std::atomic<bool> playback_done{false};
    std::atomic<bool> playback_error{false};
    std::atomic<int64_t> last_output_us{0};
    std::atomic_flag initialize_lock = ATOMIC_FLAG_INIT;
};

namespace
{

std::uint32_t read_be32(const std::uint8_t *data) noexcept
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint16_t read_le16(const std::uint8_t *data) noexcept
{
    return static_cast<std::uint16_t>(data[0] | data[1] << 8U);
}

std::uint32_t read_le32(const std::uint8_t *data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint32_t read_syncsafe32(const std::uint8_t *data) noexcept
{
    return (static_cast<std::uint32_t>(data[0] & 0x7FU) << 21U) |
           (static_cast<std::uint32_t>(data[1] & 0x7FU) << 14U) |
           (static_cast<std::uint32_t>(data[2] & 0x7FU) << 7U) |
           static_cast<std::uint32_t>(data[3] & 0x7FU);
}

std::string lower_extension(const char *path)
{
    if (path == nullptr)
        return {};
    const char *extension = std::strrchr(path, '.');
    if (extension == nullptr)
        return {};
    std::string result(extension);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
    return result;
}

MusicCodec codec_from_extension(const std::string &extension) noexcept
{
    if (extension == ".mp3")
        return MusicCodec::mp3;
    if (extension == ".wav")
        return MusicCodec::wav;
    if (extension == ".aac")
        return MusicCodec::aac;
    if (extension == ".flac")
        return MusicCodec::flac;
    if (extension == ".m4a" || extension == ".mp4")
        return MusicCodec::m4a;
    if (extension == ".opus" || extension == ".ogg")
        return MusicCodec::opus;
    if (extension == ".ts")
        return MusicCodec::ts;
    return MusicCodec::unknown;
}

esp_err_t probe_mp3(
    FILE *file,
    std::uint64_t file_size,
    MusicProbe &probe)
{
    std::uint8_t header[10]{};
    std::uint64_t offset = 0;
    if (file_size >= sizeof(header) &&
        std::fread(header, 1, sizeof(header), file) == sizeof(header) &&
        std::memcmp(header, "ID3", 3) == 0 && header[3] >= 3 && header[3] <= 4)
    {
        const std::uint64_t tag_size = read_syncsafe32(header + 6U);
        offset = 10U + tag_size + ((header[5] & 0x10U) != 0U ? 10U : 0U);
        probe.has_id3 = true;
    }

    if (offset + 4U > file_size ||
        std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
        return ESP_ERR_INVALID_SIZE;

    constexpr std::uint32_t kMaxScanBytes = 256U * 1024U;
    const std::uint64_t scan_end = std::min<std::uint64_t>(
        file_size - 4U, offset + kMaxScanBytes);
    std::uint8_t frame[4]{};
    for (std::uint64_t position = offset; position <= scan_end; ++position)
    {
        if (std::fseek(file, static_cast<long>(position), SEEK_SET) != 0 ||
            std::fread(frame, 1, sizeof(frame), file) != sizeof(frame))
            break;
        const std::uint32_t value = read_be32(frame);
        if ((value & 0xFFE00000U) != 0xFFE00000U)
            continue;
        const std::uint32_t version = (value >> 19U) & 0x3U;
        const std::uint32_t layer = (value >> 17U) & 0x3U;
        const std::uint32_t bitrate_index = (value >> 12U) & 0xFU;
        const std::uint32_t sample_index = (value >> 10U) & 0x3U;
        if (version == 1U || layer != 1U || bitrate_index == 0U ||
            bitrate_index == 15U || sample_index >= 3U)
            continue;

        constexpr int kMpeg1Bitrates[] = {
            32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
        constexpr int kMpeg2Bitrates[] = {
            8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
        constexpr int kMpeg1Rates[] = {44100, 48000, 32000};
        constexpr int kMpeg2Rates[] = {22050, 24000, 16000};
        constexpr int kMpeg25Rates[] = {11025, 12000, 8000};
        const bool mpeg1 = version == 3U;
        const int *bitrates = mpeg1 ? kMpeg1Bitrates : kMpeg2Bitrates;
        const int *rates = version == 2U ? kMpeg2Rates :
                           (version == 0U ? kMpeg25Rates : kMpeg1Rates);
        const int bitrate = bitrates[bitrate_index - 1U] * 1000;
        const int sample_rate = rates[sample_index];
        const std::uint8_t channels =
            ((value >> 6U) & 0x3U) == 3U ? 1U : 2U;
        probe.format.codec = MusicCodec::mp3;
        probe.format.sample_rate_hz = sample_rate;
        probe.format.bitrate_bps = bitrate;
        probe.format.channels = channels;
        probe.format.bits_per_sample = 16U;
        const std::uint64_t audio_bytes = file_size - position;
        probe.duration_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            audio_bytes * 8000ULL / static_cast<std::uint64_t>(bitrate),
            std::numeric_limits<std::uint32_t>::max()));
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t probe_wav(
    FILE *file,
    std::uint64_t file_size,
    MusicProbe &probe)
{
    std::uint8_t header[12]{};
    if (file_size < sizeof(header) ||
        std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8U, "WAVE", 4) != 0)
        return ESP_ERR_NOT_SUPPORTED;

    bool found_format = false;
    bool found_data = false;
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t data_size = 0;
    std::uint64_t offset = 12U;
    while (offset + 8U <= file_size)
    {
        std::uint8_t chunk[8]{};
        if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
            std::fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk))
            return ESP_ERR_INVALID_SIZE;
        const std::uint32_t size = read_le32(chunk + 4U);
        const std::uint64_t payload = offset + 8U;
        if (payload + size > file_size)
            return ESP_ERR_INVALID_SIZE;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16U)
        {
            std::uint8_t format[16]{};
            if (std::fseek(file, static_cast<long>(payload), SEEK_SET) != 0 ||
                std::fread(format, 1, sizeof(format), file) != sizeof(format))
                return ESP_ERR_INVALID_SIZE;
            if (read_le16(format) != 1U && read_le16(format) != 3U)
                return ESP_ERR_NOT_SUPPORTED;
            channels = read_le16(format + 2U);
            sample_rate = read_le32(format + 4U);
            bits = read_le16(format + 14U);
            found_format = channels != 0U && sample_rate != 0U && bits != 0U;
        }
        else if (std::memcmp(chunk, "data", 4) == 0 && !found_data)
        {
            data_size = size;
            found_data = true;
        }
        offset = payload + size + (size & 1U);
    }
    if (!found_format || !found_data)
        return ESP_ERR_NOT_SUPPORTED;
    const std::uint64_t byte_rate =
        static_cast<std::uint64_t>(sample_rate) * channels * bits / 8U;
    if (byte_rate == 0U)
        return ESP_ERR_INVALID_SIZE;
    probe.format.codec = MusicCodec::wav;
    probe.format.sample_rate_hz = static_cast<std::int32_t>(sample_rate);
    probe.format.bitrate_bps = static_cast<std::int32_t>(byte_rate * 8U);
    probe.format.channels = static_cast<std::uint8_t>(channels);
    probe.format.bits_per_sample = static_cast<std::uint8_t>(bits);
    probe.duration_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(data_size) * 1000ULL / byte_rate,
        std::numeric_limits<std::uint32_t>::max()));
    return ESP_OK;
}

esp_err_t probe_flac(
    FILE *file,
    std::uint64_t file_size,
    MusicProbe &probe)
{
    std::uint8_t stream_info[42]{};
    if (file_size < sizeof(stream_info) || std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fread(stream_info, 1, sizeof(stream_info), file) != sizeof(stream_info) ||
        std::memcmp(stream_info, "fLaC", 4) != 0 ||
        (stream_info[4] & 0x7FU) != 0U ||
        ((static_cast<std::uint32_t>(stream_info[5]) << 16U) |
         (static_cast<std::uint32_t>(stream_info[6]) << 8U) |
         stream_info[7]) < 34U)
        return ESP_ERR_NOT_SUPPORTED;
    const std::uint32_t sample_rate =
        (static_cast<std::uint32_t>(stream_info[18]) << 12U) |
        (static_cast<std::uint32_t>(stream_info[19]) << 4U) |
        (stream_info[20] >> 4U);
    const std::uint8_t channels =
        static_cast<std::uint8_t>(((stream_info[20] >> 1U) & 0x7U) + 1U);
    const std::uint8_t bits = static_cast<std::uint8_t>(
        (((stream_info[20] & 0x1U) << 4U) | (stream_info[21] >> 4U)) + 1U);
    const std::uint64_t samples =
        (static_cast<std::uint64_t>(stream_info[21] & 0x0FU) << 32U) |
        (static_cast<std::uint64_t>(stream_info[22]) << 24U) |
        (static_cast<std::uint64_t>(stream_info[23]) << 16U) |
        (static_cast<std::uint64_t>(stream_info[24]) << 8U) |
        stream_info[25];
    if (sample_rate == 0U || samples == 0U)
        return ESP_ERR_NOT_SUPPORTED;
    probe.format.codec = MusicCodec::flac;
    probe.format.sample_rate_hz = static_cast<std::int32_t>(sample_rate);
    probe.format.channels = channels;
    probe.format.bits_per_sample = bits;
    probe.format.bitrate_bps = static_cast<std::int32_t>(
        static_cast<std::uint64_t>(file_size) * 8ULL * sample_rate / samples);
    probe.duration_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        samples * 1000ULL / sample_rate,
        std::numeric_limits<std::uint32_t>::max()));
    return ESP_OK;
}

esp_err_t probe_aac(
    FILE *file,
    std::uint64_t file_size,
    MusicProbe &probe)
{
    std::uint8_t header[7]{};
    if (file_size < sizeof(header) || std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 0xFFU || (header[1] & 0xF6U) != 0xF0U)
        return ESP_ERR_NOT_SUPPORTED;
    constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                              24000, 22050, 16000, 12000, 11025, 8000, 7350};
    const std::uint32_t rate_index = (header[2] >> 2U) & 0x0FU;
    if (rate_index >= sizeof(kRates) / sizeof(kRates[0]))
        return ESP_ERR_NOT_SUPPORTED;
    const std::uint8_t channels = static_cast<std::uint8_t>(
        ((header[2] & 0x1U) << 2U) | (header[3] >> 6U));
    if (channels == 0U)
        return ESP_ERR_NOT_SUPPORTED;
    probe.format.codec = MusicCodec::aac;
    probe.format.sample_rate_hz = kRates[rate_index];
    probe.format.channels = channels;
    probe.format.bits_per_sample = 16U;
    return ESP_OK;
}

esp_err_t probe_file(const char *path, MusicProbe &probe)
{
    probe = MusicProbe{};
    if (path == nullptr)
        return ESP_ERR_INVALID_ARG;
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
        return ESP_ERR_NOT_FOUND;
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    const long size = std::ftell(file);
    const std::uint64_t file_size = size < 0 ? 0U : static_cast<std::uint64_t>(size);
    const std::string extension = lower_extension(path);
    std::uint8_t magic[4]{};
    std::fseek(file, 0, SEEK_SET);
    const std::size_t magic_size = std::fread(magic, 1, sizeof(magic), file);
    esp_err_t result = ESP_ERR_NOT_SUPPORTED;
    if (magic_size == sizeof(magic) && std::memcmp(magic, "RIFF", 4) == 0)
        result = probe_wav(file, file_size, probe);
    else if (magic_size == sizeof(magic) && std::memcmp(magic, "fLaC", 4) == 0)
        result = probe_flac(file, file_size, probe);
    else if (extension == ".mp3")
        result = probe_mp3(file, file_size, probe);
    else if (extension == ".aac")
        result = probe_aac(file, file_size, probe);
    else
        probe.format.codec = codec_from_extension(extension);
    std::fclose(file);
    return result == ESP_OK || probe.format.codec != MusicCodec::unknown
               ? ESP_OK
               : result;
}

void notify_event(MusicRuntime &rt, const MusicEvent &event)
{
    portENTER_CRITICAL(&rt.lock);
    const MusicEventCallback callback = rt.callback;
    void *const context = rt.callback_context;
    portEXIT_CRITICAL(&rt.lock);
    if (callback != nullptr)
        callback(event, context);
}

void set_state(MusicRuntime &rt, MusicState state, esp_err_t error = ESP_OK)
{
    rt.state.store(state);
    rt.playing.store(state == MusicState::playing || state == MusicState::paused);
    rt.paused.store(state == MusicState::paused);
    if (error != ESP_OK)
        rt.last_error.store(error);
    MusicEvent event{};
    event.type = error == ESP_OK ? MusicEventType::state_changed : MusicEventType::error;
    event.state = state;
    event.format = rt.format.load();
    event.position_ms = rt.position_ms.load();
    event.duration_ms = rt.duration_ms.load();
    event.error = error;
    portENTER_CRITICAL(&rt.lock);
    std::memcpy(event.path.data(), rt.path, event.path.size());
    portEXIT_CRITICAL(&rt.lock);
    notify_event(rt, event);
}

int player_output_cb(std::uint8_t *data, int data_size, void *context)
{
    auto *rt = static_cast<MusicRuntime *>(context);
    if (rt == nullptr || data == nullptr || data_size <= 0 || rt->board == nullptr)
        return ESP_FAIL;
    if (rt->playback_done.load())
        return ESP_FAIL;

    rt->output_callback_active.store(true);
    rt->last_output_us.store(esp_timer_get_time());
    ++rt->output_calls;
    std::size_t total = 0U;
    const int64_t deadline = esp_timer_get_time() +
                             static_cast<int64_t>(kOutputTimeoutMs) * 1000LL;
    esp_err_t result = ESP_OK;
    while (total < static_cast<std::size_t>(data_size))
    {
        std::size_t written = 0U;
        const std::uint32_t remaining_ms =
            esp_timer_get_time() >= deadline
                ? 0U
                : static_cast<std::uint32_t>((deadline - esp_timer_get_time()) / 1000LL);
        result = rt->board->writeAudio(data + total,
                                       static_cast<std::size_t>(data_size) - total,
                                       written,
                                       remaining_ms);
        total += written;
        rt->output_bytes.fetch_add(static_cast<std::uint32_t>(written));
        if (written == 0U || result != ESP_OK)
            break;
        if (total < static_cast<std::size_t>(data_size))
            ++rt->output_short_writes;
    }
    rt->output_callback_active.store(false);
    if (result != ESP_OK || total != static_cast<std::size_t>(data_size))
    {
        ++rt->output_errors;
        rt->playback_error.store(true);
        return ESP_FAIL;
    }
    return 0;
}

void update_duration(MusicRuntime &rt)
{
    if (rt.duration_ms.load() != 0U)
        return;
    const MusicFormat info = rt.format.load();
    if (info.bitrate_bps <= 0)
        return;
    char path[kPathBytes]{};
    portENTER_CRITICAL(&rt.lock);
    std::memcpy(path, rt.path, sizeof(path));
    portEXIT_CRITICAL(&rt.lock);
    struct stat file_info{};
    if (stat(path, &file_info) != 0 || file_info.st_size <= 0)
        return;
    const std::uint64_t duration =
        static_cast<std::uint64_t>(file_info.st_size) * 8000ULL /
        static_cast<std::uint64_t>(info.bitrate_bps);
    rt.duration_ms.store(static_cast<std::uint32_t>(std::min<std::uint64_t>(
        duration, std::numeric_limits<std::uint32_t>::max())));
}

int player_event_cb(esp_asp_event_pkt_t *event, void *context)
{
    auto *rt = static_cast<MusicRuntime *>(context);
    if (rt == nullptr || event == nullptr || event->payload == nullptr ||
        event->payload_size <= 0)
        return 0;

    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
        event->payload_size >= static_cast<int>(sizeof(esp_asp_music_info_t)))
    {
        esp_asp_music_info_t info{};
        std::memcpy(&info, event->payload, sizeof(info));
        MusicFormat format{};
        format.codec = rt->format.load().codec;
        format.sample_rate_hz = info.sample_rate;
        format.bitrate_bps = info.bitrate;
        format.channels = info.channels;
        format.bits_per_sample = info.bits;
        rt->format.store(format);
        if (rt->board != nullptr &&
            format.sample_rate_hz !=
                static_cast<std::int32_t>(rt->board->audioConfig().sample_rate_hz))
        {
            ESP_LOGW(kTag,
                     "music format %ld Hz does not match BSP output %lu Hz; "
                     "resampling must be enabled for this file",
                     static_cast<long>(format.sample_rate_hz),
                     static_cast<unsigned long>(rt->board->audioConfig().sample_rate_hz));
#if !CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
            // With resampling disabled, feeding a different rate to the fixed
            // BSP I2S clock would play at the wrong speed. Let the pipeline
            // terminate through the normal error path instead.
            rt->last_error.store(ESP_ERR_NOT_SUPPORTED);
            rt->playback_error.store(true);
            rt->playback_done.store(true);
#endif
        }
        update_duration(*rt);
        MusicEvent output{};
        output.type = MusicEventType::music_info;
        output.state = rt->state.load();
        output.format = format;
        output.position_ms = rt->position_ms.load();
        output.duration_ms = rt->duration_ms.load();
        portENTER_CRITICAL(&rt->lock);
        std::memcpy(output.path.data(), rt->path, output.path.size());
        portEXIT_CRITICAL(&rt->lock);
        notify_event(*rt, output);
        return 0;
    }

    if (event->type != ESP_ASP_EVENT_TYPE_STATE ||
        event->payload_size < static_cast<int>(sizeof(esp_asp_state_t)))
        return 0;
    esp_asp_state_t state = ESP_ASP_STATE_NONE;
    std::memcpy(&state, event->payload, sizeof(state));
    if (state == ESP_ASP_STATE_FINISHED || state == ESP_ASP_STATE_STOPPED)
        rt->playback_done.store(true);
    else if (state == ESP_ASP_STATE_ERROR)
    {
        rt->playback_error.store(true);
        rt->playback_done.store(true);
    }
    return 0;
}

void complete_command(MusicRuntime &rt, const MusicCommand &command, esp_err_t result)
{
    rt.completed_result.store(result);
    rt.completed_sequence.store(command.sequence);
    if (rt.command_events != nullptr)
        xEventGroupSetBits(rt.command_events, kCommandComplete);
}

esp_err_t stop_pipeline(MusicRuntime &rt)
{
    if (rt.player == nullptr || !rt.playing.load())
        return ESP_OK;
    // Make the output callback stop accepting new buffers before asking GMF
    // to stop, so a blocked I2S write cannot keep the pipeline alive.
    rt.playback_done.store(true);
    const esp_gmf_err_t result = esp_audio_simple_player_stop(rt.player);
    const TickType_t wait_start = xTaskGetTickCount();
    while (rt.output_callback_active.load() &&
           xTaskGetTickCount() - wait_start < pdMS_TO_TICKS(kOutputTimeoutMs))
        vTaskDelay(1);
    rt.paused.store(false);
    rt.playing.store(false);
    return result == ESP_GMF_ERR_OK || result == ESP_GMF_ERR_INVALID_STATE
               ? ESP_OK
               : ESP_FAIL;
}

void playback_task(void *argument)
{
    auto &rt = *static_cast<MusicRuntime *>(argument);
    bool shutdown = false;
    bool active = false;
    int64_t last_tick_us = esp_timer_get_time();
    while (!shutdown)
    {
        MusicCommand command{};
        const TickType_t wait_ticks = active ? pdMS_TO_TICKS(20U) : portMAX_DELAY;
        if (xQueueReceive(rt.commands, &command, wait_ticks) == pdTRUE)
        {
            esp_err_t result = ESP_OK;
            switch (command.type)
            {
            case CommandType::play:
                result = stop_pipeline(rt);
                if (result == ESP_OK)
                {
                    portENTER_CRITICAL(&rt.lock);
                    std::strncpy(rt.path, command.path, sizeof(rt.path) - 1U);
                    rt.path[sizeof(rt.path) - 1U] = '\0';
                    portEXIT_CRITICAL(&rt.lock);
                    rt.position_ms.store(0U);
                    rt.duration_ms.store(command.has_probe ? command.probe.duration_ms : 0U);
                    rt.format.store(command.has_probe ? command.probe.format : MusicFormat{});
                    rt.last_error.store(ESP_OK);
                    rt.playback_done.store(false);
                    rt.playback_error.store(false);
                    set_state(rt, MusicState::starting);
                    char uri[kPathBytes + 8U]{};
                    const int length = std::snprintf(
                        uri, sizeof(uri), "file://%s",
                        rt.path[0] == '/' ? rt.path + 1 : rt.path);
                    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(uri))
                        result = ESP_ERR_INVALID_ARG;
                    else
                    {
                        const esp_gmf_err_t run_result =
                            esp_audio_simple_player_run(rt.player, uri, nullptr);
                        result = run_result == ESP_GMF_ERR_OK ? ESP_OK : ESP_FAIL;
                    }
                    if (result == ESP_OK)
                    {
                        active = true;
                        last_tick_us = esp_timer_get_time();
                        rt.last_output_us.store(last_tick_us);
                        set_state(rt, MusicState::playing);
                    }
                    else
                    {
                        rt.playback_error.store(true);
                        set_state(rt, MusicState::error, result);
                    }
                }
                break;
            case CommandType::pause:
                if (!active)
                    result = ESP_ERR_INVALID_STATE;
                else
                {
                    result = esp_audio_simple_player_pause(rt.player) == ESP_GMF_ERR_OK
                                 ? ESP_OK
                                 : ESP_FAIL;
                    if (result == ESP_OK)
                        set_state(rt, MusicState::paused);
                }
                break;
            case CommandType::resume:
                if (!active)
                    result = ESP_ERR_INVALID_STATE;
                else
                {
                    result = esp_audio_simple_player_resume(rt.player) == ESP_GMF_ERR_OK
                                 ? ESP_OK
                                 : ESP_FAIL;
                    if (result == ESP_OK)
                    {
                        last_tick_us = esp_timer_get_time();
                        set_state(rt, MusicState::playing);
                    }
                }
                break;
            case CommandType::stop:
                result = stop_pipeline(rt);
                active = false;
                set_state(rt, MusicState::stopped, result == ESP_OK ? ESP_OK : result);
                break;
            case CommandType::shutdown:
                result = stop_pipeline(rt);
                active = false;
                shutdown = true;
                break;
            }
            complete_command(rt, command, result);
        }

        if (active)
        {
            if (rt.playback_done.load())
            {
                active = false;
                const esp_err_t playback_error = rt.playback_error.load()
                                                     ? (rt.last_error.load() == ESP_OK
                                                            ? ESP_FAIL
                                                            : rt.last_error.load())
                                                     : ESP_OK;
                set_state(rt, rt.playback_error.load() ? MusicState::error
                                                        : MusicState::finished,
                          playback_error);
                rt.playing.store(false);
                rt.paused.store(false);
            }
            else
            {
                const int64_t now = esp_timer_get_time();
                if (!rt.paused.load() && rt.last_output_us.load() > 0 &&
                    now - rt.last_output_us.load() >
                        static_cast<int64_t>(kPlaybackStallTimeoutMs) * 1000LL)
                {
                    ++rt.playback_timeouts;
                    rt.last_error.store(ESP_ERR_TIMEOUT);
                    (void)stop_pipeline(rt);
                    active = false;
                    set_state(rt, MusicState::error, ESP_ERR_TIMEOUT);
                    continue;
                }
                if (!rt.paused.load() && now > last_tick_us)
                    rt.position_ms.fetch_add(static_cast<std::uint32_t>(
                        (now - last_tick_us) / 1000LL));
                last_tick_us = now;
            }
        }
        rt.stack_high_watermark_words.store(
            static_cast<std::uint32_t>(uxTaskGetStackHighWaterMark(nullptr)));
    }
    rt.task_exited.store(true);
    vTaskDelete(nullptr);
}

esp_err_t ensure_runtime(MusicRuntime *out)
{
    if (out == nullptr)
        return ESP_ERR_NO_MEM;
    while (out->initialize_lock.test_and_set(std::memory_order_acquire))
        vTaskDelay(1);
    if (out->task != nullptr)
    {
        out->initialize_lock.clear(std::memory_order_release);
        return ESP_OK;
    }
    out->commands = xQueueCreate(4, sizeof(MusicCommand));
    out->command_mutex = xSemaphoreCreateMutex();
    out->command_events = xEventGroupCreate();
    if (out->commands == nullptr || out->command_mutex == nullptr ||
        out->command_events == nullptr)
    {
        if (out->commands != nullptr)
            vQueueDelete(out->commands);
        if (out->command_mutex != nullptr)
            vSemaphoreDelete(out->command_mutex);
        if (out->command_events != nullptr)
            vEventGroupDelete(out->command_events);
        out->commands = nullptr;
        out->command_mutex = nullptr;
        out->command_events = nullptr;
        out->initialize_lock.clear(std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    esp_asp_cfg_t config{};
    config.out.cb = player_output_cb;
    config.out.user_ctx = out;
    config.task_prio = 8;
    config.task_stack = 16 * 1024;
    config.task_core = 1;
    config.task_stack_in_ext = false;
    if (esp_audio_simple_player_new(&config, &out->player) != ESP_GMF_ERR_OK)
    {
        vQueueDelete(out->commands);
        vSemaphoreDelete(out->command_mutex);
        vEventGroupDelete(out->command_events);
        out->commands = nullptr;
        out->command_mutex = nullptr;
        out->command_events = nullptr;
        out->initialize_lock.clear(std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    if (esp_audio_simple_player_set_event(out->player, player_event_cb, out) !=
        ESP_GMF_ERR_OK)
    {
        (void)esp_audio_simple_player_destroy(out->player);
        out->player = nullptr;
        vQueueDelete(out->commands);
        vSemaphoreDelete(out->command_mutex);
        vEventGroupDelete(out->command_events);
        out->commands = nullptr;
        out->command_mutex = nullptr;
        out->command_events = nullptr;
        out->initialize_lock.clear(std::memory_order_release);
        return ESP_FAIL;
    }
    out->task_exited.store(false);
    if (xTaskCreate(playback_task, "p4_music", 8192, out, 10, &out->task) != pdPASS)
    {
        (void)esp_audio_simple_player_destroy(out->player);
        out->player = nullptr;
        vQueueDelete(out->commands);
        vSemaphoreDelete(out->command_mutex);
        vEventGroupDelete(out->command_events);
        out->commands = nullptr;
        out->command_mutex = nullptr;
        out->command_events = nullptr;
        out->initialize_lock.clear(std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    out->initialize_lock.clear(std::memory_order_release);
    return ESP_OK;
}

esp_err_t submit_command(MusicRuntime &rt, CommandType type, const char *path,
                         std::uint32_t timeout_ms,
                         const MusicProbe *probe = nullptr)
{
    if (rt.task == nullptr || rt.commands == nullptr || rt.command_mutex == nullptr)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(rt.command_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        ++rt.command_timeouts;
        return ESP_ERR_TIMEOUT;
    }
    MusicCommand command{};
    command.type = type;
    command.sequence = rt.next_sequence.fetch_add(1U) + 1U;
    if (path != nullptr)
    {
        const std::size_t length = std::strlen(path);
        if (length >= sizeof(command.path))
        {
            xSemaphoreGive(rt.command_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        std::memcpy(command.path, path, length + 1U);
    }
    if (probe != nullptr)
    {
        command.probe = *probe;
        command.has_probe = true;
    }
    xEventGroupClearBits(rt.command_events, kCommandComplete);
    if (xQueueSend(rt.commands, &command, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        xSemaphoreGive(rt.command_mutex);
        ++rt.command_timeouts;
        return ESP_ERR_TIMEOUT;
    }
    const TickType_t start = xTaskGetTickCount();
    esp_err_t result = ESP_ERR_TIMEOUT;
    for (;;)
    {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (timeout_ms != portMAX_DELAY && elapsed >= pdMS_TO_TICKS(timeout_ms))
            break;
        const TickType_t remaining = timeout_ms == portMAX_DELAY
                                         ? portMAX_DELAY
                                         : pdMS_TO_TICKS(timeout_ms) - elapsed;
        xEventGroupWaitBits(rt.command_events, kCommandComplete, pdTRUE, pdFALSE,
                            remaining);
        if (rt.completed_sequence.load() == command.sequence)
        {
            result = rt.completed_result.load();
            break;
        }
    }
    xSemaphoreGive(rt.command_mutex);
    if (result == ESP_ERR_TIMEOUT)
        ++rt.command_timeouts;
    if (result != ESP_OK)
        rt.last_error.store(result);
    return result;
}
} // namespace

MusicRuntime *Board::ensureMusicRuntime()
{
    if (music_runtime_ == nullptr)
        music_runtime_ = new (std::nothrow) MusicRuntime{};
    if (music_runtime_ != nullptr)
        music_runtime_->board = this;
    return music_runtime_;
}

esp_err_t read_id3_metadata(const char *path, std::vector<std::uint8_t> *cover,
                            std::string *lyrics)
{
    if (path == nullptr || (cover == nullptr && lyrics == nullptr))
        return ESP_ERR_INVALID_ARG;
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
        return ESP_ERR_NOT_FOUND;
    std::uint8_t header[10]{};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, "ID3", 3) != 0 || header[3] < 3 || header[3] > 4)
    {
        std::fclose(file);
        return ESP_ERR_NOT_FOUND;
    }
    const std::uint32_t total = (std::uint32_t(header[6] & 0x7f) << 21) |
                                (std::uint32_t(header[7] & 0x7f) << 14) |
                                (std::uint32_t(header[8] & 0x7f) << 7) |
                                (header[9] & 0x7fU);
    if (total == 0U || total > kMaxId3Bytes)
    {
        std::fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }
    std::vector<std::uint8_t> tag(total);
    const std::size_t read = std::fread(tag.data(), 1, tag.size(), file);
    std::fclose(file);
    if (read != tag.size())
        return ESP_ERR_INVALID_SIZE;

    bool found = false;
    for (std::size_t offset = 0; offset + 10U <= tag.size();)
    {
        const std::uint32_t size = header[3] == 4
                                       ? (std::uint32_t(tag[offset + 4] & 0x7f) << 21) |
                                             (std::uint32_t(tag[offset + 5] & 0x7f) << 14) |
                                             (std::uint32_t(tag[offset + 6] & 0x7f) << 7) |
                                             (tag[offset + 7] & 0x7f)
                                       : (std::uint32_t(tag[offset + 4]) << 24) |
                                             (std::uint32_t(tag[offset + 5]) << 16) |
                                             (std::uint32_t(tag[offset + 6]) << 8) |
                                             tag[offset + 7];
        if (size == 0U || offset + 10U + size > tag.size())
            break;
        const std::uint8_t *data = tag.data() + offset + 10U;
        if (cover != nullptr && std::memcmp(tag.data() + offset, "APIC", 4) == 0 &&
            size > 4U)
        {
            std::size_t cursor = 1U;
            while (cursor < size && data[cursor] != 0U)
                ++cursor;
            if (cursor < size)
            {
                ++cursor;
                if (cursor < size)
                {
                    ++cursor;
                    while (cursor < size && data[cursor] != 0U)
                        ++cursor;
                    if (++cursor < size)
                        cover->assign(data + cursor, data + size);
                }
            }
            found = !cover->empty();
        }
        else if (lyrics != nullptr &&
                 std::memcmp(tag.data() + offset, "USLT", 4) == 0 && size > 5U)
        {
            // ID3v2.3/v2.4 USLT starts with encoding, language and descriptor.
            // Preserve the raw text bytes here; callers can apply their preferred
            // character-set conversion without making the BSP depend on a codec.
            std::size_t cursor = 4U;
            while (cursor < size && data[cursor] != 0U)
                ++cursor;
            if (cursor < size)
            {
                ++cursor;
                lyrics->assign(reinterpret_cast<const char *>(data + cursor),
                               size - cursor);
                found = !lyrics->empty();
            }
        }
        offset += 10U + size;
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t Board::probeMusicFile(const char *path, MusicProbe &probe) const
{
    return probe_file(path, probe);
}

esp_err_t Board::playMusicFile(const char *path)
{
    return playMusicFile(path, MusicPlaybackConfig{});
}

esp_err_t Board::playMusicFile(
    const char *path,
    const MusicPlaybackConfig &config)
{
    if (path == nullptr || !isAudioInitialized())
        return ESP_ERR_INVALID_STATE;

    MusicProbe probe{};
    // Playback always needs one internal probe so the decoder receives the
    // source format and duration. Call the internal parser directly here;
    // probeMusicFile() remains the optional public preflight API and should
    // not add another dispatch layer to the normal playback path.
    const esp_err_t probe_result = probe_file(path, probe);
    if (probe_result == ESP_ERR_NOT_FOUND)
        return probe_result;
    if (probe_result != ESP_OK && config.reject_unknown_format)
        return probe_result;
    if (config.reject_unknown_format &&
        probe.format.codec == MusicCodec::unknown)
        return ESP_ERR_NOT_SUPPORTED;

    if (config.auto_configure_audio && !config.allow_resample &&
        probe.format.sample_rate_hz > 0 &&
        static_cast<std::uint32_t>(probe.format.sample_rate_hz) !=
            audioConfig().sample_rate_hz)
    {
        AudioConfig audio_config = audioConfig();
        audio_config.sample_rate_hz =
            static_cast<std::uint32_t>(probe.format.sample_rate_hz);
        const esp_err_t reconfigure_result = reconfigureAudio(audio_config);
        if (reconfigure_result != ESP_OK)
            return reconfigure_result;
    }

    MusicRuntime *rt = nullptr;
    rt = ensureMusicRuntime();
    esp_err_t result = ensure_runtime(rt);
    if (result != ESP_OK)
        return result;
    return submit_command(
        *rt, CommandType::play, path, kCommandTimeoutMs, &probe);
}

esp_err_t Board::pauseMusic(bool paused)
{
    if (!isAudioInitialized() || music_runtime_ == nullptr)
        return ESP_ERR_INVALID_STATE;
    return submit_command(*music_runtime_, paused ? CommandType::pause
                                                   : CommandType::resume,
                          nullptr, kCommandTimeoutMs);
}

esp_err_t Board::stopMusic()
{
    return stopMusic(kCommandTimeoutMs);
}

esp_err_t Board::stopMusic(std::uint32_t timeout_ms)
{
    if (music_runtime_ == nullptr)
        return ESP_OK;
    return submit_command(*music_runtime_, CommandType::stop, nullptr, timeout_ms);
}

esp_err_t Board::deinitializeMusic()
{
    MusicRuntime *rt = music_runtime_;
    if (rt == nullptr)
        return ESP_OK;
    if (rt->task == nullptr)
    {
        if (rt->commands != nullptr)
            vQueueDelete(rt->commands);
        if (rt->command_mutex != nullptr)
            vSemaphoreDelete(rt->command_mutex);
        if (rt->command_events != nullptr)
            vEventGroupDelete(rt->command_events);
        delete rt;
        music_runtime_ = nullptr;
        return ESP_OK;
    }
    const esp_err_t result = submit_command(*rt, CommandType::shutdown, nullptr,
                                             kCommandTimeoutMs);
    if (result != ESP_OK)
        return result;
    const TickType_t start = xTaskGetTickCount();
    while (!rt->task_exited.load())
    {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(kCommandTimeoutMs))
            return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    if (rt->player != nullptr)
        (void)esp_audio_simple_player_destroy(rt->player);
    if (rt->task != nullptr)
        rt->task = nullptr;
    if (rt->commands != nullptr)
        vQueueDelete(rt->commands);
    if (rt->command_mutex != nullptr)
        vSemaphoreDelete(rt->command_mutex);
    if (rt->command_events != nullptr)
        vEventGroupDelete(rt->command_events);
    delete rt;
    music_runtime_ = nullptr;
    return ESP_OK;
}

esp_err_t Board::setMusicEventCallback(MusicEventCallback callback, void *context)
{
    MusicRuntime *rt = nullptr;
    rt = ensureMusicRuntime();
    const esp_err_t result = ensure_runtime(rt);
    if (result != ESP_OK)
        return result;
    portENTER_CRITICAL(&rt->lock);
    rt->callback = callback;
    rt->callback_context = context;
    portEXIT_CRITICAL(&rt->lock);
    return ESP_OK;
}

MusicState Board::musicState() const noexcept
{
    return music_runtime_ == nullptr ? MusicState::idle : music_runtime_->state.load();
}

MusicFormat Board::musicFormat() const noexcept
{
    return music_runtime_ == nullptr ? MusicFormat{} : music_runtime_->format.load();
}

esp_err_t Board::musicLastError() const noexcept
{
    return music_runtime_ == nullptr ? ESP_OK : music_runtime_->last_error.load();
}

const char *Board::musicLastErrorName() const noexcept
{
    return esp_err_to_name(musicLastError());
}

MusicStatistics Board::musicStatistics() const noexcept
{
    MusicStatistics result{};
    if (music_runtime_ == nullptr)
        return result;
    result.output_calls = music_runtime_->output_calls.load();
    result.output_bytes = music_runtime_->output_bytes.load();
    result.output_short_writes = music_runtime_->output_short_writes.load();
    result.output_errors = music_runtime_->output_errors.load();
    result.playback_timeouts = music_runtime_->playback_timeouts.load();
    result.command_timeouts = music_runtime_->command_timeouts.load();
    result.task_stack_high_watermark_words =
        music_runtime_->stack_high_watermark_words.load();
    return result;
}

void Board::resetMusicStatistics() noexcept
{
    if (music_runtime_ == nullptr)
        return;
    music_runtime_->output_calls.store(0);
    music_runtime_->output_bytes.store(0);
    music_runtime_->output_short_writes.store(0);
    music_runtime_->output_errors.store(0);
    music_runtime_->playback_timeouts.store(0);
    music_runtime_->command_timeouts.store(0);
}

bool Board::isMusicPlaying() const noexcept
{
    return music_runtime_ != nullptr && music_runtime_->playing.load();
}

bool Board::isMusicPaused() const noexcept
{
    return music_runtime_ != nullptr && music_runtime_->paused.load();
}

std::uint32_t Board::musicPositionMs() const noexcept
{
    return music_runtime_ == nullptr ? 0U : music_runtime_->position_ms.load();
}

std::uint32_t Board::musicDurationMs() const noexcept
{
    return music_runtime_ == nullptr ? 0U : music_runtime_->duration_ms.load();
}

esp_err_t Board::seekMusicMs(std::uint32_t)
{
    return music_runtime_ == nullptr ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t Board::getMusicCover(std::vector<std::uint8_t> &cover) const
{
    cover.clear();
    if (music_runtime_ == nullptr)
        return ESP_ERR_INVALID_STATE;
    char path[kPathBytes]{};
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&music_runtime_->lock));
    std::memcpy(path, music_runtime_->path, sizeof(path));
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&music_runtime_->lock));
    return read_id3_metadata(path, &cover, nullptr);
}

esp_err_t Board::getMusicLyrics(std::string &lyrics) const
{
    lyrics.clear();
    if (music_runtime_ == nullptr)
        return ESP_ERR_INVALID_STATE;
    char path[kPathBytes]{};
    portENTER_CRITICAL(const_cast<portMUX_TYPE *>(&music_runtime_->lock));
    std::memcpy(path, music_runtime_->path, sizeof(path));
    portEXIT_CRITICAL(const_cast<portMUX_TYPE *>(&music_runtime_->lock));
    return read_id3_metadata(path, nullptr, &lyrics);
}

esp_err_t Board::getMusicCover(const char *path, std::vector<std::uint8_t> &cover) const
{
    cover.clear();
    return path == nullptr ? ESP_ERR_INVALID_ARG : read_id3_metadata(path, &cover, nullptr);
}

esp_err_t Board::getMusicLyrics(const char *path, std::string &lyrics) const
{
    lyrics.clear();
    return path == nullptr ? ESP_ERR_INVALID_ARG : read_id3_metadata(path, nullptr, &lyrics);
}

} // namespace bsp::t_panel_p4_rect
