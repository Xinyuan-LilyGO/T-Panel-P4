#include "page_common.hpp"
#include "page_registry.hpp"
#include "libs/tjpgd/tjpgd.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>
#include <new>
#include <memory>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <lvgl_page_manager.h>
#include <services/service_context.hpp>
#include <services/settings_control.h>
#include <bsp/board.hpp>

extern "C" const lv_image_dsc_t _album_art_RGB565A8_240x240;

namespace factory::ui
{
    namespace
    {
        constexpr lv_coord_t kScreenSize = 720;
        constexpr lv_coord_t kCoverSize = 240;
        constexpr lv_coord_t kParsedCoverSize = 70;
        constexpr lv_coord_t kCoverTop = 80;
        constexpr lv_coord_t kProgressWidth = 396;
        constexpr lv_coord_t kProgressTop = 480;
        constexpr lv_coord_t kControlTop = 544;
        constexpr std::size_t kMaxTracks = 32;
        constexpr std::size_t kPlaylistRows = 10;
        constexpr lv_coord_t kPlaylistWidth = 448;
        constexpr lv_coord_t kPlaylistHeight = 448;
        constexpr lv_coord_t kPlaylistPadding = 16;
        constexpr lv_coord_t kPlaylistHeaderHeight = 60;
        constexpr lv_coord_t kPlaylistViewportWidth = kPlaylistWidth - 2 * (kPlaylistPadding + 1);
        constexpr lv_coord_t kPlaylistViewportHeight = kPlaylistHeight - 2 * (kPlaylistPadding + 1) - kPlaylistHeaderHeight;
        constexpr lv_coord_t kPlaylistRowHeight = 50;
        constexpr lv_coord_t kPlaylistRowStride = 58;
        constexpr lv_coord_t kPlaylistTextLeft = 42;
        constexpr lv_coord_t kPlaylistDurationWidth = 52;
        constexpr lv_coord_t kPlaylistTextWidth = kPlaylistViewportWidth - kPlaylistTextLeft - kPlaylistDurationWidth - 20;
        constexpr std::size_t kPathSize = 192;
        constexpr std::size_t kTextSize = 96;
        constexpr char kMusicDirectory[] = "/sdcard/music";

        struct MusicTrack
        {
            char path[kPathSize]{};
            char title[kTextSize]{};
            char artist[kTextSize]{};
            std::uint32_t duration_ms{};
        };

        struct TrackCacheDeleter
        {
            void operator()(MusicTrack *tracks) const noexcept
            {
                if (tracks == nullptr)
                    return;
                for (std::size_t index = 0U; index < kMaxTracks; ++index)
                    tracks[index].~MusicTrack();
                heap_caps_free(tracks);
            }
        };
        using TrackCache = std::unique_ptr<MusicTrack[], TrackCacheDeleter>;

        TrackCache allocateTrackCache()
        {
            auto *tracks = static_cast<MusicTrack *>(heap_caps_malloc(
                kMaxTracks * sizeof(MusicTrack), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (tracks != nullptr)
            {
                for (std::size_t index = 0U; index < kMaxTracks; ++index)
                    ::new (static_cast<void *>(tracks + index)) MusicTrack{};
            }
            return TrackCache{tracks};
        }

        enum class ScanResultType
        {
            track,
            metadata,
            directory_done,
            complete,
            cover
        };

        struct ScanResult
        {
            ScanResultType type{};
            std::size_t index{};
            MusicTrack track{};
            std::uint16_t *pixels{}; // Ownership transfers through the result queue.
            char *lyrics{};          // UTF-8 text; ownership transfers with the pixels.
            std::uint32_t cover_request{};
            bool directory_opened{};
        };

        struct CoverRequest
        {
            char path[kPathSize]{};
            std::uint32_t id{};
        };

        struct MusicLoader
        {
            QueueHandle_t results{};
            QueueHandle_t covers{};
            bsp::t_panel_p4_round::Board *board{};
            std::atomic<bool> cancelled{false};
            std::atomic<unsigned> references{2}; // Page and worker each own one reference.
            TrackCache tracks;
        };

        // The BSP callback outlives individual LVGL events. It only reads this
        // protected path snapshot, never the page tree or loader's storage.
        portMUX_TYPE s_playlist_lock = portMUX_INITIALIZER_UNLOCKED;
        enum class PlaybackOrder
        {
            sequence,
            random,
            loop
        };
        struct PlaybackQueue
        {
            char paths[kMaxTracks][kPathSize]{};
            std::size_t count{};
            std::size_t index{};
            bool active{};
            PlaybackOrder order{PlaybackOrder::sequence};
        } s_playback;

        struct PlaylistRow
        {
            lv_obj_t *row{};
            lv_obj_t *play_circle{};
            lv_obj_t *play_icon{};
            lv_obj_t *title{};
            lv_obj_t *artist{};
            lv_obj_t *duration{};
        };

        struct LyricLine
        {
            std::uint32_t time_ms{};
            std::string text;
        };

        struct MusicUi
        {
            lv_obj_t *root{};
            lv_obj_t *song_name{};
            lv_obj_t *play_button{};
            lv_obj_t *play_icon{};
            lv_obj_t *order_label{};
            PlaybackOrder order{PlaybackOrder::sequence};
            lv_obj_t *progress{};
            lv_obj_t *elapsed{};
            lv_obj_t *duration{};
            lv_obj_t *artist{};
            lv_obj_t *lyric{};
            lv_obj_t *lyric_next{};
            lv_obj_t *volume{};
            lv_obj_t *volume_icon{};
            lv_obj_t *volume_min_icon{};
            lv_obj_t *cover_image{};
            lv_obj_t *parsed_cover_image{};
            // lv_obj_t *cover_placeholder{};
            lv_draw_buf_t *cover_buf{};
            bool cover_rotating{};
            std::vector<LyricLine> lyrics;
            bool lyrics_timed{};
            lv_obj_t *playlist_panel{};
            lv_obj_t *playlist_status{};
            lv_obj_t *playlist_list{};
            lv_obj_t *playlist_spacer{};
            PlaylistRow playlist_rows[kPlaylistRows]{};
            lv_timer_t *timer{};
            std::size_t track_count{};
            std::size_t current_index{};
            std::size_t displayed_index{kMaxTracks};
            std::uint32_t displayed_position{};
            TrackCache tracks;
            MusicLoader *loader{};
            bool loader_started{};
            bool scanning{true};
            bool directory_opened{};
            bool load_failed{};
            std::size_t metadata_count{};
            std::size_t cover_index{kMaxTracks};
            std::uint32_t cover_request{};
        };
        MusicUi s_music;

        bsp::t_panel_p4_round::Board *musicBoard()
        {
            return factory::services::board();
        }

        const char *trackTitle(std::size_t index)
        {
            return index < s_music.track_count && s_music.tracks[index].title[0] != '\0'
                       ? s_music.tracks[index].title
                       : "No music files";
        }

        void formatTime(std::uint32_t milliseconds, char *buffer, std::size_t size)
        {
            if (buffer == nullptr || size == 0U)
                return;
            const std::uint32_t seconds = milliseconds / 1000U;
            std::snprintf(buffer, size, "%02u:%02u",
                          static_cast<unsigned>(seconds / 60U),
                          static_cast<unsigned>(seconds % 60U));
        }

        void setLabelIfChanged(lv_obj_t *label, const char *text)
        {
            if (label == nullptr || text == nullptr)
                return;
            const char *current = lv_label_get_text(label);
            if (current == nullptr || std::strcmp(current, text) != 0)
                lv_label_set_text(label, text);
        }

        std::uint32_t readTagSize(const std::uint8_t *data, bool syncsafe)
        {
            std::uint32_t value = 0U;
            for (unsigned index = 0; index < 4; ++index)
                value = (value << (syncsafe ? 7U : 8U)) |
                        (data[index] & (syncsafe ? 0x7fU : 0xffU));
            return value;
        }

        void decodeTagText(const std::uint8_t *data, std::size_t size, char *output)
        {
            if (size < 2U)
                return;
            const unsigned encoding = *data++;
            --size;
            std::string text;
            if (encoding == 3U)
                text.assign(reinterpret_cast<const char *>(data), size);
            else if (encoding <= 2U)
            {
                bool little_endian = false;
                std::size_t offset = 0U;
                if (encoding == 1U && size >= 2U)
                {
                    little_endian = data[0] == 0xffU && data[1] == 0xfeU;
                    if (little_endian || (data[0] == 0xfeU && data[1] == 0xffU))
                        offset = 2U;
                }
                while (offset < size)
                {
                    std::uint32_t codepoint = data[offset++];
                    if (encoding != 0U)
                    {
                        if (offset >= size)
                            break;
                        codepoint = little_endian ? codepoint | (data[offset] << 8U)
                                                  : (codepoint << 8U) | data[offset];
                        ++offset;
                        if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
                            codepoint = 0xfffdU;
                    }
                    if (codepoint == 0U)
                        break;
                    if (codepoint < 0x80U)
                        text.push_back(static_cast<char>(codepoint));
                    else if (codepoint < 0x800U)
                    {
                        text.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
                        text.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                    }
                    else
                    {
                        text.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
                        text.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
                        text.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
                    }
                }
            }
            const std::size_t terminator = text.find('\0');
            if (terminator != std::string::npos)
                text.resize(terminator);
            if (text.empty())
                return; // Preserve the filename/unknown-artist fallback.
            std::size_t count = text.size() < kTextSize - 1U ? text.size() : kTextSize - 1U;
            while (count < text.size() && count > 0U &&
                   (static_cast<unsigned char>(text[count]) & 0xc0U) == 0x80U)
                --count;
            std::memcpy(output, text.data(), count);
            output[count] = '\0';
        }

        void readTrackMetadata(MusicTrack &track, const MusicLoader &loader)
        {
            FILE *file = std::fopen(track.path, "rb");
            if (file == nullptr)
                return;
            std::uint8_t header[10]{};
            if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
                std::memcmp(header, "ID3", 3) != 0 || header[3] < 3U || header[3] > 4U ||
                (header[5] & 0x80U) != 0U)
            {
                std::fclose(file);
                return;
            }
            const bool version4 = header[3] == 4U;
            const std::uint32_t tag_size = readTagSize(header + 6, true);
            std::uint32_t offset = 0U;
            if ((header[5] & 0x40U) != 0U)
            {
                std::uint8_t extended[4]{};
                if (std::fread(extended, 1, 4, file) == 4U)
                {
                    const std::uint32_t extended_size = readTagSize(extended, version4);
                    const std::uint32_t prefix_size = version4 ? 0U : 4U;
                    offset = tag_size >= prefix_size && extended_size <= tag_size - prefix_size
                                 ? extended_size + prefix_size
                                 : tag_size;
                }
                else
                    offset = tag_size;
            }
            // Bound malformed tags and seek over pictures instead of reading them.
            for (unsigned frames = 0; frames < 256U && !loader.cancelled.load() &&
                                      tag_size <= 4U * 1024U * 1024U &&
                                      offset <= tag_size && tag_size - offset >= 10U;
                 ++frames)
            {
                std::uint8_t frame[10]{};
                if (std::fseek(file, static_cast<long>(10U + offset), SEEK_SET) != 0 ||
                    std::fread(frame, 1, 10, file) != 10U || frame[0] == 0U)
                    break;
                const std::uint32_t size = readTagSize(frame + 4, version4);
                if (size == 0U || size > tag_size - offset - 10U)
                    break;
                char *output = std::memcmp(frame, "TIT2", 4) == 0   ? track.title
                               : std::memcmp(frame, "TPE1", 4) == 0 ? track.artist
                                                                    : nullptr;
                if (output != nullptr && frame[9] == 0U)
                {
                    std::uint8_t payload[512]{};
                    const std::size_t count = size < sizeof(payload) ? size : sizeof(payload);
                    if (std::fread(payload, 1, count, file) == count)
                        decodeTagText(payload, count, output);
                }
                offset += 10U + size;
            }
            std::fclose(file);
        }

        void releaseLoader(MusicLoader *loader)
        {
            if (loader == nullptr || loader->references.fetch_sub(1) != 1U)
                return;
            ScanResult pending{};
            while (xQueueReceive(loader->results, &pending, 0) == pdTRUE)
            {
                std::free(pending.pixels);
                std::free(pending.lyrics);
            }
            vQueueDelete(loader->results);
            vQueueDelete(loader->covers);
            delete loader;
        }

        bool publishResult(MusicLoader &loader, const ScanResult &result)
        {
            while (!loader.cancelled.load())
            {
                if (xQueueSend(loader.results, &result, pdMS_TO_TICKS(20)) == pdTRUE)
                    return true;
            }
            std::free(result.pixels);
            std::free(result.lyrics);
            return false;
        }

        void loaderTask(void *argument);

        void startLoader()
        {
            s_music.loader_started = true;
            s_music.tracks = allocateTrackCache();
            auto *loader = s_music.tracks != nullptr ? new (std::nothrow) MusicLoader{} : nullptr;
            if (loader != nullptr)
            {
                loader->tracks = allocateTrackCache();
                loader->results = xQueueCreate(8, sizeof(ScanResult));
                loader->covers = xQueueCreate(1, sizeof(CoverRequest));
                loader->board = musicBoard();
                if (loader->tracks != nullptr && loader->results != nullptr && loader->covers != nullptr &&
                    xTaskCreate(loaderTask, "music_scan", 8192, loader, 1, nullptr) == pdPASS)
                {
                    s_music.loader = loader;
                    return;
                }
                if (loader->results != nullptr)
                    vQueueDelete(loader->results);
                if (loader->covers != nullptr)
                    vQueueDelete(loader->covers);
                delete loader;
            }
            s_music.tracks.reset();
            s_music.scanning = false;
            s_music.load_failed = true;
        }

        bool musicNextTrack(char *path, std::size_t path_size, void *)
        {
            if (path == nullptr || path_size == 0U)
                return false;
            const std::uint32_t random = esp_random();
            portENTER_CRITICAL(&s_playlist_lock);
            const bool available = s_playback.active && s_playback.count > 0U;
            if (available)
            {
                if (s_playback.order == PlaybackOrder::random && s_playback.count > 1U)
                    s_playback.index = (s_playback.index + 1U + random % (s_playback.count - 1U)) % s_playback.count;
                else if (s_playback.order != PlaybackOrder::loop)
                    s_playback.index = (s_playback.index + 1U) % s_playback.count;
                std::snprintf(path, path_size, "%s", s_playback.paths[s_playback.index]);
            }
            portEXIT_CRITICAL(&s_playlist_lock);
            return available;
        }

        void updateCover();

        void selectPlaybackTrack()
        {
            portENTER_CRITICAL(&s_playlist_lock);
            s_playback.index = s_music.current_index;
            portEXIT_CRITICAL(&s_playlist_lock);
        }

        void updateTrackLabels()
        {
            const MusicTrack *track = s_music.current_index < s_music.track_count
                                          ? &s_music.tracks[s_music.current_index]
                                          : nullptr;
            if (s_music.song_name != nullptr)
                setLabelIfChanged(s_music.song_name, track != nullptr      ? track->title
                                                     : s_music.load_failed ? "Unable to start scan"
                                                     : s_music.scanning    ? "Scanning music..."
                                                                           : "No music files");
            if (s_music.artist != nullptr)
                setLabelIfChanged(s_music.artist, track != nullptr   ? track->artist
                                                  : s_music.scanning ? "Reading SD card..."
                                                                     : "Add audio files to /sdcard/music");
            if (s_music.duration != nullptr)
            {
                char duration[12]{};
                formatTime(track != nullptr ? track->duration_ms : 0U, duration, sizeof(duration));
                setLabelIfChanged(s_music.duration, duration);
            }
            updateCover();
        }

        struct JpegContext
        {
            const MusicLoader *loader{};
            unsigned blocks{};
            const std::uint8_t *data{};
            std::size_t size{};
            std::size_t offset{};
            std::uint16_t *pixels{};
            std::uint32_t source_width{};
            std::uint32_t source_height{};
            std::uint32_t target_x{};
            std::uint32_t target_y{};
            std::uint32_t target_width{};
            std::uint32_t target_height{};
            std::uint32_t target_stride_pixels{};
        };

        std::size_t jpegInput(JDEC *decoder, std::uint8_t *buffer, std::size_t size)
        {
            auto *context = static_cast<JpegContext *>(decoder != nullptr ? decoder->device : nullptr);
            if (context == nullptr || context->offset > context->size)
                return 0U;
            if (buffer == nullptr)
            {
                if (size > context->size - context->offset)
                    return 0U;
                context->offset += size;
                return size;
            }
            const std::size_t count = (size < context->size - context->offset)
                                          ? size
                                          : context->size - context->offset;
            std::memcpy(buffer, context->data + context->offset, count);
            context->offset += count;
            return count;
        }

        int jpegOutput(JDEC *decoder, void *bitmap, JRECT *rect)
        {
            auto *context = static_cast<JpegContext *>(decoder != nullptr ? decoder->device : nullptr);
            if (context == nullptr || bitmap == nullptr || rect == nullptr ||
                context->source_width == 0U || context->source_height == 0U)
                return 0;
            if (context->loader->cancelled.load())
                return 0;
            if (++context->blocks % 32U == 0U)
                vTaskDelay(1);
            const auto *source = static_cast<const std::uint8_t *>(bitmap);
            const std::uint32_t block_width = static_cast<std::uint32_t>(rect->right - rect->left + 1U);
            for (std::uint32_t y = rect->top; y <= rect->bottom; ++y)
            {
                const std::uint32_t target_y = context->target_y +
                                               (y * context->target_height) / context->source_height;
                if (target_y >= kParsedCoverSize)
                    continue;
                for (std::uint32_t x = rect->left; x <= rect->right; ++x)
                {
                    const std::uint32_t target_x = context->target_x +
                                                   (x * context->target_width) / context->source_width;
                    if (target_x >= kParsedCoverSize)
                        continue;
                    const std::uint8_t *pixel = source +
                                                ((y - rect->top) * block_width + (x - rect->left)) * 3U;
                    context->pixels[target_y * context->target_stride_pixels + target_x] =
                        static_cast<std::uint16_t>(((pixel[2] >> 3U) << 11U) |
                                                   ((pixel[1] >> 2U) << 5U) | (pixel[0] >> 3U));
                }
            }
            return 1;
        }

        std::uint16_t *decodeJpeg(const std::uint8_t *data, std::size_t size,
                                  const MusicLoader &loader)
        {
            if (data == nullptr || size < 4U || data[0] != 0xffU || data[1] != 0xd8U)
                return nullptr;
            auto *pixels = static_cast<std::uint16_t *>(
                std::calloc(kParsedCoverSize * kParsedCoverSize, sizeof(std::uint16_t)));
            if (pixels == nullptr)
                return nullptr;
            JpegContext context{};
            context.loader = &loader;
            context.data = data;
            context.size = size;
            context.pixels = pixels;
            context.target_stride_pixels = kParsedCoverSize;
            auto *work = static_cast<std::uint8_t *>(std::malloc(4096U));
            if (work == nullptr)
            {
                std::free(pixels);
                return nullptr;
            }
            JDEC decoder{};
            if (jd_prepare(&decoder, jpegInput, work, 4096U, &context) != JDR_OK ||
                decoder.width == 0U || decoder.height == 0U)
            {
                std::free(work);
                std::free(pixels);
                return nullptr;
            }
            context.source_width = decoder.width;
            context.source_height = decoder.height;
            const std::uint32_t scale_x = kParsedCoverSize * decoder.height;
            const std::uint32_t scale_y = kParsedCoverSize * decoder.width;
            if (scale_x < scale_y)
            {
                context.target_width = kParsedCoverSize;
                context.target_height = scale_x / decoder.width;
            }
            else
            {
                context.target_height = kParsedCoverSize;
                context.target_width = scale_y / decoder.height;
            }
            context.target_width = context.target_width == 0U ? 1U : context.target_width;
            context.target_height = context.target_height == 0U ? 1U : context.target_height;
            context.target_x = (kParsedCoverSize - context.target_width) / 2U;
            context.target_y = (kParsedCoverSize - context.target_height) / 2U;
            const JRESULT result = jd_decomp(&decoder, jpegOutput, 0U);
            std::free(work);
            if (result != JDR_OK)
            {
                std::free(pixels);
                return nullptr;
            }
            return pixels;
        }

        void loadRequestedCover(MusicLoader &loader, TickType_t wait = 0)
        {
            CoverRequest request{};
            if (loader.cancelled.load() || xQueueReceive(loader.covers, &request, wait) != pdTRUE)
                return;
            ScanResult result{};
            result.type = ScanResultType::cover;
            result.cover_request = request.id;
            if (loader.board != nullptr)
            {
                std::vector<std::uint8_t> cover;
                std::string lyrics;
                (void)loader.board->getMusicMetadata(request.path, cover, lyrics);
                if (!loader.cancelled.load())
                {
                    // Bound the text transferred to the UI, including malformed tags.
                    std::size_t count = std::min(lyrics.size(), std::size_t{64U * 1024U});
                    while (count < lyrics.size() && count > 0U &&
                           (static_cast<unsigned char>(lyrics[count]) & 0xc0U) == 0x80U)
                        --count;
                    if (count > 0U)
                    {
                        result.lyrics = static_cast<char *>(std::malloc(count + 1U));
                        if (result.lyrics != nullptr)
                        {
                            std::memcpy(result.lyrics, lyrics.data(), count);
                            result.lyrics[count] = '\0';
                        }
                    }
                    result.pixels = decodeJpeg(cover.data(), cover.size(), loader);
                }
            }
            (void)publishResult(loader, result);
        }

        void loaderTask(void *argument)
        {
            auto *loader = static_cast<MusicLoader *>(argument);
            std::size_t count = 0U;
            DIR *directory = opendir(kMusicDirectory);
            const bool opened = directory != nullptr;
            if (directory != nullptr)
            {
                struct dirent *entry = nullptr;
                while (!loader->cancelled.load() && count < kMaxTracks &&
                       (entry = readdir(directory)) != nullptr)
                {
                    vTaskDelay(1);
                    if (entry->d_type == DT_DIR || entry->d_name[0] == '.')
                        continue;
                    const char *extension = std::strrchr(entry->d_name, '.');
                    if (extension == nullptr ||
                        (strcasecmp(extension, ".mp3") != 0 && strcasecmp(extension, ".wav") != 0 &&
                         strcasecmp(extension, ".flac") != 0 && strcasecmp(extension, ".aac") != 0 &&
                         strcasecmp(extension, ".m4a") != 0 && strcasecmp(extension, ".opus") != 0))
                        continue;
                    MusicTrack &track = loader->tracks[count];
                    const std::size_t name_size = std::strlen(entry->d_name);
                    if (sizeof(kMusicDirectory) + name_size + 1U > sizeof(track.path))
                        continue;
                    std::snprintf(track.path, sizeof(track.path), "%s/%s", kMusicDirectory, entry->d_name);
                    std::size_t title_size = static_cast<std::size_t>(extension - entry->d_name);
                    if (title_size >= sizeof(track.title))
                        title_size = sizeof(track.title) - 1U;
                    while (title_size > 0U &&
                           (static_cast<unsigned char>(entry->d_name[title_size]) & 0xc0U) == 0x80U)
                        --title_size;
                    std::memcpy(track.title, entry->d_name, title_size);
                    track.title[title_size] = '\0';
                    std::snprintf(track.artist, sizeof(track.artist), "%s", "Unknown artist");
                    ScanResult result{};
                    result.type = ScanResultType::track;
                    result.index = count++;
                    result.track = track;
                    if (!publishResult(*loader, result))
                        break;
                    vTaskDelay(1);
                }
                closedir(directory);
            }
            ScanResult state{};
            state.type = ScanResultType::directory_done;
            state.directory_opened = opened;
            (void)publishResult(*loader, state);

            // All filenames have been published before any audio-file parsing.
            for (std::size_t index = 0U; index < count && !loader->cancelled.load(); ++index)
            {
                loadRequestedCover(*loader);
                MusicTrack &track = loader->tracks[index];
                if (loader->board != nullptr)
                {
                    bsp::t_panel_p4_round::MusicProbe probe{};
                    if (loader->board->probeMusicFile(track.path, probe) == ESP_OK)
                        track.duration_ms = probe.duration_ms;
                }
                if (loader->cancelled.load())
                    break;
                readTrackMetadata(track, *loader);
                ScanResult result{};
                result.type = ScanResultType::metadata;
                result.index = index;
                result.track = track;
                if (!publishResult(*loader, result))
                    break;
                vTaskDelay(1);
            }
            state.type = ScanResultType::complete;
            (void)publishResult(*loader, state);
            // Remain available for selected-track cover requests until page exit.
            while (!loader->cancelled.load())
                loadRequestedCover(*loader, pdMS_TO_TICKS(50));
            releaseLoader(loader);
            vTaskDelete(nullptr);
        }

        void coverRotateExec(void *object, int32_t angle);

        void releaseCover()
        {
            if (s_music.parsed_cover_image != nullptr)
                lv_anim_del(s_music.parsed_cover_image, coverRotateExec);
            s_music.cover_rotating = false;
            if (s_music.cover_image != nullptr)
                lv_image_set_src(s_music.cover_image, &_album_art_RGB565A8_240x240);
            if (s_music.parsed_cover_image != nullptr)
                lv_image_set_src(s_music.parsed_cover_image, nullptr);
            if (s_music.cover_buf != nullptr)
                lv_draw_buf_destroy(s_music.cover_buf);
            s_music.cover_buf = nullptr;
        }

        void coverRotateExec(void *object, int32_t angle)
        {
            if (object != nullptr)
                lv_obj_set_style_transform_angle(static_cast<lv_obj_t *>(object), angle, LV_PART_MAIN);
        }

        void updateCoverRotation(bool playing)
        {
            if (s_music.parsed_cover_image == nullptr || s_music.cover_buf == nullptr)
                return;
            if (!playing)
            {
                if (s_music.cover_rotating)
                {
                    lv_anim_del(s_music.parsed_cover_image, coverRotateExec);
                    s_music.cover_rotating = false;
                }
                return;
            }
            if (s_music.cover_rotating)
                return;
            lv_anim_t animation{};
            lv_anim_init(&animation);
            lv_anim_set_var(&animation, s_music.parsed_cover_image);
            lv_anim_set_values(&animation,
                               lv_obj_get_style_transform_angle(s_music.parsed_cover_image, LV_PART_MAIN),
                               lv_obj_get_style_transform_angle(s_music.parsed_cover_image, LV_PART_MAIN) + 3600);
            lv_anim_set_duration(&animation, 24000);
            lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&animation, lv_anim_path_linear);
            lv_anim_set_exec_cb(&animation, coverRotateExec);
            lv_anim_start(&animation);
            s_music.cover_rotating = true;
        }

        void renderLyrics(std::uint32_t position_ms)
        {
            if (s_music.lyrics.empty())
                return;
            std::size_t next = 1U;
            const char *current = s_music.lyrics.front().text.c_str();
            if (s_music.lyrics_timed)
            {
                next = 0U;
                while (next < s_music.lyrics.size() && s_music.lyrics[next].time_ms <= position_ms)
                    ++next;
                current = next > 0U ? s_music.lyrics[next - 1U].text.c_str() : "";
            }
            setLabelIfChanged(s_music.lyric, current);
            setLabelIfChanged(s_music.lyric_next,
                              next < s_music.lyrics.size() ? s_music.lyrics[next].text.c_str() : "");
        }

        bool parseLyricTime(const std::string &tag, std::uint32_t &time_ms)
        {
            const char *cursor = tag.c_str();
            std::uint32_t minutes = 0U;
            unsigned digits = 0U;
            while (*cursor >= '0' && *cursor <= '9' && digits < 6U)
            {
                minutes = minutes * 10U + static_cast<unsigned>(*cursor++ - '0');
                ++digits;
            }
            if (digits == 0U || *cursor != ':')
                return false;
            ++cursor;
            if (std::strlen(cursor) < 2U || cursor[0] < '0' || cursor[0] > '5' ||
                cursor[1] < '0' || cursor[1] > '9')
                return false;
            const unsigned seconds = (cursor[0] - '0') * 10U + cursor[1] - '0';
            cursor += 2;
            unsigned fraction = 0U;
            if (*cursor == '.')
            {
                ++cursor;
                unsigned multiplier = 100U;
                digits = 0U;
                while (*cursor >= '0' && *cursor <= '9' && digits < 3U)
                {
                    fraction += static_cast<unsigned>(*cursor++ - '0') * multiplier;
                    multiplier /= 10U;
                    ++digits;
                }
                if (digits == 0U)
                    return false;
            }
            const std::uint64_t value = static_cast<std::uint64_t>(minutes) * 60000U +
                                        seconds * 1000U + fraction;
            if (*cursor != '\0' || value > UINT32_MAX)
                return false;
            time_ms = static_cast<std::uint32_t>(value);
            return true;
        }

        void setLyrics(const char *text)
        {
            s_music.lyrics.clear();
            s_music.lyrics_timed = false;
            const std::string lyrics = text != nullptr ? text : "";
            std::vector<LyricLine> plain;
            for (std::size_t start = 0U; start < lyrics.size() && s_music.lyrics.size() < 512U;)
            {
                const std::size_t end = lyrics.find_first_of("\r\n", start);
                std::string line = lyrics.substr(start, end == std::string::npos ? end : end - start);
                start = end == std::string::npos ? lyrics.size() : end + 1U;
                if (line.compare(0, 3, "\xef\xbb\xbf") == 0)
                    line.erase(0, 3);
                std::vector<std::uint32_t> times;
                std::size_t offset = 0U;
                while (offset < line.size() && line[offset] == '[')
                {
                    const std::size_t close = line.find(']', offset);
                    if (close == std::string::npos)
                        break;
                    std::uint32_t time = 0U;
                    if (parseLyricTime(line.substr(offset + 1U, close - offset - 1U), time))
                        times.push_back(time);
                    else if (line.find(':', offset) >= close)
                        break; // Preserve bracketed text that is not LRC metadata.
                    offset = close + 1U;
                }
                line.erase(0, offset);
                for (std::uint32_t time : times)
                {
                    if (s_music.lyrics.size() >= 512U)
                        break;
                    s_music.lyrics.push_back({time, line});
                }
                if (times.empty() && !line.empty() && plain.size() < 512U)
                    plain.push_back({0U, std::move(line)});
            }
            s_music.lyrics_timed = !s_music.lyrics.empty();
            if (s_music.lyrics_timed)
                std::stable_sort(s_music.lyrics.begin(), s_music.lyrics.end(),
                                 [](const LyricLine &a, const LyricLine &b)
                                 { return a.time_ms < b.time_ms; });
            else
                s_music.lyrics = std::move(plain);
            setLabelIfChanged(s_music.lyric, s_music.lyrics.empty() ? "No lyrics available" : "");
            setLabelIfChanged(s_music.lyric_next, "");
            renderLyrics(0U);
        }

        void updateCover()
        {
            if (s_music.loader == nullptr || s_music.current_index >= s_music.track_count)
            {
                s_music.cover_index = kMaxTracks;
                return;
            }
            if (s_music.cover_index == s_music.current_index)
                return;
            s_music.cover_index = s_music.current_index;
            releaseCover();
            setLyrics(nullptr);
            setLabelIfChanged(s_music.lyric, "Loading lyrics...");
            // if (s_music.cover_placeholder != nullptr)
            //     lv_obj_clear_flag(s_music.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
            CoverRequest request{};
            std::snprintf(request.path, sizeof(request.path), "%s",
                          s_music.tracks[s_music.current_index].path);
            request.id = ++s_music.cover_request;
            xQueueOverwrite(s_music.loader->covers, &request);
        }

        void refreshPlaylist();

        void applyLoaderResults()
        {
            if (!s_music.loader_started)
                startLoader(); // First timer tick runs after the page can be rendered.
            bool playlist_changed = false;
            bool selected_changed = s_music.load_failed;
            ScanResult result{};
            // Bound each UI tick; the worker waits on its bounded queue when full.
            for (unsigned batch = 0; s_music.loader != nullptr && batch < 4U &&
                                     xQueueReceive(s_music.loader->results, &result, 0) == pdTRUE;
                 ++batch)
            {
                if ((result.type == ScanResultType::track || result.type == ScanResultType::metadata) &&
                    result.index < kMaxTracks)
                {
                    s_music.tracks[result.index] = result.track;
                    if (result.type == ScanResultType::track)
                    {
                        s_music.track_count = result.index + 1U;
                        portENTER_CRITICAL(&s_playlist_lock);
                        std::memcpy(s_playback.paths[result.index], result.track.path, kPathSize);
                        s_playback.count = s_music.track_count;
                        portEXIT_CRITICAL(&s_playlist_lock);
                    }
                    else
                        ++s_music.metadata_count;
                    playlist_changed = true;
                    selected_changed |= result.index == s_music.current_index;
                }
                else if (result.type == ScanResultType::directory_done)
                {
                    s_music.scanning = false;
                    s_music.directory_opened = result.directory_opened;
                    playlist_changed = selected_changed = true;
                }
                else if (result.type == ScanResultType::complete)
                    playlist_changed = true;
                else if (result.type == ScanResultType::cover)
                {
                    if (result.cover_request == s_music.cover_request)
                        setLyrics(result.lyrics);
                    if (result.pixels != nullptr && result.cover_request == s_music.cover_request)
                    {
                        releaseCover();
                        s_music.cover_buf = lv_draw_buf_create(kParsedCoverSize, kParsedCoverSize,
                                                               LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
                        if (s_music.cover_buf != nullptr)
                        {
                            for (unsigned row = 0; row < kParsedCoverSize; ++row)
                                std::memcpy(s_music.cover_buf->data + row * s_music.cover_buf->header.stride,
                                            result.pixels + row * kParsedCoverSize,
                                            kParsedCoverSize * sizeof(std::uint16_t));
                            if (s_music.parsed_cover_image != nullptr)
                            {
                                lv_image_set_src(s_music.parsed_cover_image, s_music.cover_buf);
                                lv_obj_clear_flag(s_music.parsed_cover_image, LV_OBJ_FLAG_HIDDEN);
                            }
                            // lv_obj_add_flag(s_music.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
                        }
                    }
                    std::free(result.pixels);
                    std::free(result.lyrics);
                }
            }
            if (selected_changed)
                updateTrackLabels();
            if (playlist_changed || s_music.load_failed)
                refreshPlaylist();
        }

        void setTextColor(lv_obj_t *object, uint32_t color, lv_opa_t opacity = LV_OPA_COVER)
        {
            if (object == nullptr)
                return;
            lv_obj_set_style_text_color(object, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_text_opa(object, opacity, LV_PART_MAIN);
        }

        lv_obj_t *makeCircle(lv_obj_t *parent, lv_coord_t size, uint32_t color, lv_opa_t opacity)
        {
            lv_obj_t *circle = lv_obj_create(parent);
            if (circle == nullptr)
                return nullptr;
            lv_obj_set_size(circle, size, size);
            lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(circle, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(circle, opacity, LV_PART_MAIN);
            lv_obj_set_style_border_width(circle, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
            lv_obj_remove_flag(circle, static_cast<lv_obj_flag_t>(
                                           LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            return circle;
        }

        void createCover(lv_obj_t *parent)
        {
            lv_obj_t *cover = makeCircle(parent, kCoverSize, 0xdde8ff, LV_OPA_COVER);
            if (cover == nullptr)
                return;
            lv_obj_align(cover, LV_ALIGN_TOP_MID, 0, kCoverTop);
            lv_obj_set_style_border_width(cover, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(cover, lv_color_hex(UI_BLACK), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_color(cover, lv_color_hex(0xa9a8c7), LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(cover, lv_color_hex(UI_BLACK), LV_PART_MAIN);
            lv_obj_set_style_shadow_width(cover, 16, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(cover, LV_OPA_30, LV_PART_MAIN);

            s_music.cover_image = lv_image_create(cover);
            lv_obj_set_size(s_music.cover_image, kCoverSize, kCoverSize);
            lv_obj_set_style_radius(s_music.cover_image, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_image_set_inner_align(s_music.cover_image, LV_IMAGE_ALIGN_CENTER);
            lv_obj_center(s_music.cover_image);
            lv_obj_remove_flag(s_music.cover_image, static_cast<lv_obj_flag_t>(
                                                        LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_image_set_src(s_music.cover_image, &_album_art_RGB565A8_240x240);

            s_music.parsed_cover_image = lv_image_create(cover);
            lv_obj_set_size(s_music.parsed_cover_image, kParsedCoverSize, kParsedCoverSize);
            lv_obj_set_style_radius(s_music.parsed_cover_image, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_x(s_music.parsed_cover_image, kParsedCoverSize / 2, LV_PART_MAIN);
            lv_obj_set_style_transform_pivot_y(s_music.parsed_cover_image, kParsedCoverSize / 2, LV_PART_MAIN);
            lv_image_set_inner_align(s_music.parsed_cover_image, LV_IMAGE_ALIGN_CENTER);
            lv_obj_center(s_music.parsed_cover_image);
            lv_obj_remove_flag(s_music.parsed_cover_image, static_cast<lv_obj_flag_t>(
                                                               LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_clear_flag(s_music.parsed_cover_image, LV_OBJ_FLAG_HIDDEN);

            // s_music.cover_placeholder = lv_label_create(cover);
            // lv_label_set_text(s_music.cover_placeholder, LV_SYMBOL_AUDIO);
            // lv_obj_set_style_text_font(s_music.cover_placeholder, &lv_font_montserrat_28, LV_PART_MAIN);
            // setTextColor(s_music.cover_placeholder, UI_TEXT, LV_OPA_100);
            // lv_obj_center(s_music.cover_placeholder);
            // lv_obj_add_flag(s_music.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
        }

        lv_obj_t *makeControlButton(lv_obj_t *parent, const char *symbol,
                                    lv_coord_t x, lv_coord_t size,
                                    lv_event_cb_t callback)
        {
            lv_obj_t *button = lv_obj_create(parent);
            if (button == nullptr)
                return nullptr;
            lv_obj_set_size(button, size, size);
            lv_obj_align(button, LV_ALIGN_LEFT_MID, x, 0);
            page_ui_apply_icon_style(button);
            lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
            if (callback != nullptr)
                lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);

            lv_obj_t *icon = lv_label_create(button);
            lv_label_set_text(icon, symbol);
            lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, LV_PART_MAIN);
            setTextColor(icon, UI_WIHTE);
            lv_obj_center(icon);
            return button;
        }

        void formatElapsed(lv_obj_t *label, int32_t seconds)
        {
            if (label == nullptr)
                return;
            if (seconds < 0)
                seconds = 0;
            const int minutes = seconds / 60;
            const int remainder = seconds % 60;
            char text[12]{};
            std::snprintf(text, sizeof(text), "%02d:%02d", minutes, remainder);
            lv_label_set_text(label, text);
        }

        void progressChanged(lv_event_t *event)
        {
            if (event == nullptr || s_music.elapsed == nullptr)
                return;
            const lv_event_code_t code = lv_event_get_code(event);
            if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
                code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
                return;
            const int32_t position = lv_slider_get_value(lv_event_get_target_obj(event));
            formatElapsed(s_music.elapsed, position);
            if (code == LV_EVENT_RELEASED)
            {
                if (bsp::t_panel_p4_round::Board *board = musicBoard(); board != nullptr && position >= 0)
                    (void)board->seekMusicMs(static_cast<std::uint32_t>(position) * 1000U);
            }
        }

        void playClicked(lv_event_t *)
        {
            bsp::t_panel_p4_round::Board *board = musicBoard();
            if (board == nullptr || s_music.play_icon == nullptr)
                return;
            if (board->isMusicPlaying())
            {
                (void)board->pauseMusic(!board->isMusicPaused());
            }
            else if (s_music.track_count > 0U)
            {
                selectPlaybackTrack();
                (void)board->playMusicFile(s_music.tracks[s_music.current_index].path);
            }
        }

        void orderClicked(lv_event_t *)
        {
            s_music.order = s_music.order == PlaybackOrder::sequence ? PlaybackOrder::random
                            : s_music.order == PlaybackOrder::random ? PlaybackOrder::loop
                                                                     : PlaybackOrder::sequence;
            portENTER_CRITICAL(&s_playlist_lock);
            s_playback.order = s_music.order;
            portEXIT_CRITICAL(&s_playlist_lock);
            setLabelIfChanged(s_music.order_label,
                              s_music.order == PlaybackOrder::sequence ? "SEQ"
                              : s_music.order == PlaybackOrder::random ? "RND"
                                                                       : "LP");
        }

        void resetTrack(lv_event_t *)
        {
            if (s_music.track_count == 0U)
                return;
            s_music.current_index = s_music.current_index == 0U
                                        ? s_music.track_count - 1U
                                        : s_music.current_index - 1U;
            selectPlaybackTrack();
            if (bsp::t_panel_p4_round::Board *board = musicBoard(); board != nullptr)
                (void)board->playMusicFile(s_music.tracks[s_music.current_index].path);
            updateTrackLabels();
            refreshPlaylist();
        }

        void nextTrack(lv_event_t *)
        {
            if (s_music.track_count == 0U)
                return;
            const std::size_t step = s_music.order == PlaybackOrder::random && s_music.track_count > 1U
                                         ? 1U + esp_random() % (s_music.track_count - 1U)
                                         : 1U;
            s_music.current_index = (s_music.current_index + step) % s_music.track_count;
            selectPlaybackTrack();
            if (bsp::t_panel_p4_round::Board *board = musicBoard(); board != nullptr)
                (void)board->playMusicFile(s_music.tracks[s_music.current_index].path);
            updateTrackLabels();
            refreshPlaylist();
        }

        void volumeChanged(lv_event_t *event)
        {
            if (event == nullptr || s_music.volume == nullptr)
                return;
            const lv_event_code_t code = lv_event_get_code(event);
            if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
                code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
                return;
            const int32_t value = lv_arc_get_value(lv_event_get_target_obj(event));
            if (code == LV_EVENT_RELEASED)
            {
                if (settings_control_set_audio_volume(static_cast<std::uint8_t>(value)) != ESP_OK)
                {
                    std::uint8_t current{};
                    if (settings_control_get_audio_volume(&current) == ESP_OK)
                    {
                        lv_arc_set_value(s_music.volume, current);
                    }
                }
            }
        }

        void refreshPlayback(lv_timer_t *)
        {
            portENTER_CRITICAL(&s_playlist_lock);
            s_music.current_index = s_playback.index;
            portEXIT_CRITICAL(&s_playlist_lock);
            applyLoaderResults();
            bsp::t_panel_p4_round::Board *board = musicBoard();
            if (board == nullptr)
                return;

            const std::uint32_t position = board->musicPositionMs();
            const std::uint32_t duration = board->musicDurationMs();
            if (s_music.current_index != s_music.displayed_index)
            {
                s_music.displayed_index = s_music.current_index;
                updateTrackLabels();
                refreshPlaylist();
            }
            renderLyrics(position);
            if (s_music.progress != nullptr && !lv_slider_is_dragged(s_music.progress))
            {
                if (duration > 0U && lv_slider_get_max_value(s_music.progress) !=
                                         static_cast<int32_t>(duration / 1000U))
                    lv_slider_set_range(s_music.progress, 0, static_cast<int32_t>(duration / 1000U));
                const std::int32_t position_seconds = static_cast<std::int32_t>(position / 1000U);
                if (position_seconds != s_music.displayed_position)
                {
                    s_music.displayed_position = static_cast<std::uint32_t>(position_seconds);
                    lv_slider_set_value(s_music.progress, position_seconds, LV_ANIM_OFF);
                    formatElapsed(s_music.elapsed, position_seconds);
                }
            }
            if (s_music.duration != nullptr && duration > 0U)
            {
                char text[12]{};
                formatTime(duration, text, sizeof(text));
                setLabelIfChanged(s_music.duration, text);
            }
            const bool playing = board->isMusicPlaying() && !board->isMusicPaused();
            if (s_music.play_icon != nullptr)
            {
                setLabelIfChanged(s_music.play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            }
            updateCoverRotation(playing);
        }

        void playlistSelect(lv_event_t *event)
        {
            if (event == nullptr)
                return;
            const std::size_t slot = reinterpret_cast<std::size_t>(lv_event_get_user_data(event));
            const lv_coord_t scroll_y = s_music.playlist_list != nullptr
                                            ? lv_obj_get_scroll_y(s_music.playlist_list)
                                            : 0;
            const std::size_t first = scroll_y > 0
                                          ? static_cast<std::size_t>(scroll_y) /
                                                static_cast<std::size_t>(kPlaylistRowStride)
                                          : 0U;
            const std::size_t index = first + slot;
            if (index >= s_music.track_count)
                return;
            s_music.current_index = index;
            selectPlaybackTrack();
            updateTrackLabels();
            if (bsp::t_panel_p4_round::Board *board = musicBoard(); board != nullptr)
                (void)board->playMusicFile(s_music.tracks[index].path);
            if (s_music.playlist_panel != nullptr)
                lv_obj_add_flag(s_music.playlist_panel, LV_OBJ_FLAG_HIDDEN);
        }

        void playlistScroll(lv_event_t *event)
        {
            if (event != nullptr && lv_event_get_code(event) == LV_EVENT_SCROLL)
                refreshPlaylist();
        }

        void togglePlaylist(lv_event_t *)
        {
            if (s_music.playlist_panel == nullptr)
                return;
            if (lv_obj_has_flag(s_music.playlist_panel, LV_OBJ_FLAG_HIDDEN))
                lv_obj_clear_flag(s_music.playlist_panel, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(s_music.playlist_panel, LV_OBJ_FLAG_HIDDEN);
        }

        void createPlaylist(lv_obj_t *parent)
        {
            s_music.playlist_panel = lv_obj_create(parent);
            if (s_music.playlist_panel == nullptr)
                return;
            lv_obj_set_size(s_music.playlist_panel, kPlaylistWidth, kPlaylistHeight);
            lv_obj_align(s_music.playlist_panel, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_radius(s_music.playlist_panel, 22, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_music.playlist_panel, lv_color_hex(0x111426), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_music.playlist_panel, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_music.playlist_panel, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(s_music.playlist_panel, lv_color_hex(0x34384e), LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_music.playlist_panel, kPlaylistPadding, LV_PART_MAIN);
            lv_obj_set_scrollbar_mode(s_music.playlist_panel, LV_SCROLLBAR_MODE_OFF);
            lv_obj_remove_flag(s_music.playlist_panel, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(s_music.playlist_panel, LV_OBJ_FLAG_HIDDEN);

            lv_obj_t *heading = page_ui_label(s_music.playlist_panel, "播放列表",
                                              &lv_font_source_han_sans_sc_16_cjk, UI_WIHTE);
            lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_set_style_text_font(heading, &lv_font_source_han_sans_sc_16_cjk, LV_PART_MAIN);
            s_music.playlist_status = page_ui_label(s_music.playlist_panel, "",
                                                    &lv_font_source_han_sans_sc_16_cjk, 0x8d92a8);
            lv_obj_align(s_music.playlist_status, LV_ALIGN_TOP_LEFT, 0, 25);

            lv_obj_t *close = makeControlButton(s_music.playlist_panel, LV_SYMBOL_CLOSE,
                                                0, 34, togglePlaylist);
            if (close != nullptr)
            {
                lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
                lv_obj_set_style_text_font(lv_obj_get_child(close, 0), &lv_font_montserrat_16, LV_PART_MAIN);
            }

            s_music.playlist_list = lv_obj_create(s_music.playlist_panel);
            if (s_music.playlist_list == nullptr)
                return;
            lv_obj_set_size(s_music.playlist_list, kPlaylistViewportWidth, kPlaylistViewportHeight);
            lv_obj_align(s_music.playlist_list, LV_ALIGN_TOP_LEFT, 0, kPlaylistHeaderHeight);
            lv_obj_set_style_bg_opa(s_music.playlist_list, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_music.playlist_list, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_music.playlist_list, 0, LV_PART_MAIN);
            lv_obj_set_scrollbar_mode(s_music.playlist_list, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_scroll_dir(s_music.playlist_list, LV_DIR_VER);
            lv_obj_add_flag(s_music.playlist_list, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(s_music.playlist_list, playlistScroll, LV_EVENT_SCROLL, nullptr);

            s_music.playlist_spacer = lv_obj_create(s_music.playlist_list);
            lv_obj_set_pos(s_music.playlist_spacer, 0, 0);
            lv_obj_set_size(s_music.playlist_spacer, 1, kPlaylistRowStride);
            lv_obj_set_style_bg_opa(s_music.playlist_spacer, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_music.playlist_spacer, 0, LV_PART_MAIN);
            lv_obj_remove_flag(s_music.playlist_spacer,
                               static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

            for (std::size_t slot = 0; slot < kPlaylistRows; ++slot)
            {
                PlaylistRow &cached = s_music.playlist_rows[slot];
                cached.row = lv_obj_create(s_music.playlist_list);
                lv_obj_set_size(cached.row, kPlaylistViewportWidth, kPlaylistRowHeight);
                lv_obj_set_pos(cached.row, 0, static_cast<lv_coord_t>(slot * kPlaylistRowStride));
                lv_obj_set_style_bg_color(cached.row, lv_color_hex(0x262a40), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(cached.row, LV_OPA_COVER, LV_STATE_PRESSED);
                lv_obj_set_style_bg_opa(cached.row, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_radius(cached.row, 16, LV_PART_MAIN);
                lv_obj_set_style_border_width(cached.row, 0, LV_PART_MAIN);
                lv_obj_set_style_pad_all(cached.row, 0, LV_PART_MAIN);
                lv_obj_set_scrollbar_mode(cached.row, LV_SCROLLBAR_MODE_OFF);
                lv_obj_remove_flag(cached.row, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(cached.row, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
                lv_obj_add_event_cb(cached.row, playlistSelect, LV_EVENT_CLICKED,
                                    reinterpret_cast<void *>(slot));

                cached.play_circle = makeCircle(cached.row, 30, 0x10d8f5, LV_OPA_COVER);
                lv_obj_align(cached.play_circle, LV_ALIGN_LEFT_MID, 0, 0);
                cached.play_icon = page_ui_label(cached.play_circle, LV_SYMBOL_PLAY,
                                                 &lv_font_montserrat_16, UI_BLACK);
                lv_obj_center(cached.play_icon);
                cached.title = page_ui_label(cached.row, "", &lv_font_source_han_sans_sc_16_cjk, UI_WIHTE);
                lv_obj_set_width(cached.title, kPlaylistTextWidth);
                lv_label_set_long_mode(cached.title, LV_LABEL_LONG_MODE_DOTS);
                lv_obj_align(cached.title, LV_ALIGN_TOP_LEFT, kPlaylistTextLeft, 3);
                cached.artist = page_ui_label(cached.row, "", &lv_font_source_han_sans_sc_16_cjk, 0x8d92a8);
                lv_obj_set_width(cached.artist, kPlaylistTextWidth);
                lv_label_set_long_mode(cached.artist, LV_LABEL_LONG_MODE_DOTS);
                lv_obj_align(cached.artist, LV_ALIGN_TOP_LEFT, kPlaylistTextLeft, 27);
                cached.duration = page_ui_label(cached.row, "--:--", &lv_font_montserrat_14, 0x8d92a8);
                lv_obj_set_width(cached.duration, kPlaylistDurationWidth);
                lv_obj_set_style_text_align(cached.duration, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
                lv_obj_align(cached.duration, LV_ALIGN_RIGHT_MID, -8, 0);
            }
        }

        void updatePlaylistRow(std::size_t slot, std::size_t index)
        {
            if (slot >= kPlaylistRows)
                return;
            PlaylistRow &cached = s_music.playlist_rows[slot];
            if (cached.row == nullptr)
                return;
            if (index >= s_music.track_count)
            {
                lv_obj_add_flag(cached.row, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_clear_flag(cached.row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(cached.row, static_cast<lv_coord_t>(index * kPlaylistRowStride));
            const bool selected = index == s_music.current_index;
            lv_obj_set_style_bg_color(cached.row, lv_color_hex(selected ? 0x262a40 : 0x111426), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cached.row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(cached.play_circle, selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
            setLabelIfChanged(cached.play_icon, selected ? LV_SYMBOL_PLAY : "");
            setLabelIfChanged(cached.title, trackTitle(index));
            setLabelIfChanged(cached.artist, s_music.tracks[index].artist);
            char duration[12] = "--:--";
            if (s_music.tracks[index].duration_ms > 0U)
                formatTime(s_music.tracks[index].duration_ms, duration, sizeof(duration));
            setLabelIfChanged(cached.duration, duration);
        }

        void refreshPlaylist()
        {
            if (s_music.playlist_list == nullptr)
                return;
            char count[64]{};
            std::snprintf(count, sizeof(count), "Local Music · %u Tracks",
                          static_cast<unsigned>(s_music.track_count));
            if (s_music.load_failed)
                setLabelIfChanged(s_music.playlist_status, "Unable to start music scan");
            else if (s_music.scanning)
                setLabelIfChanged(s_music.playlist_status, "Scanning SD card...");
            else if (!s_music.directory_opened)
                setLabelIfChanged(s_music.playlist_status, "Unable to read /sdcard/music");
            else if (s_music.metadata_count < s_music.track_count)
            {
                std::snprintf(count, sizeof(count), "%u Tracks · Info %u/%u",
                              static_cast<unsigned>(s_music.track_count),
                              static_cast<unsigned>(s_music.metadata_count),
                              static_cast<unsigned>(s_music.track_count));
                setLabelIfChanged(s_music.playlist_status, count);
            }
            else
                setLabelIfChanged(s_music.playlist_status, count);
            const lv_coord_t scroll_y = lv_obj_get_scroll_y(s_music.playlist_list);
            const std::size_t first = scroll_y > 0
                                          ? static_cast<std::size_t>(scroll_y) /
                                                static_cast<std::size_t>(kPlaylistRowStride)
                                          : 0U;
            for (std::size_t slot = 0; slot < kPlaylistRows; ++slot)
                updatePlaylistRow(slot, first + slot);
            const std::size_t content_rows = s_music.track_count > kPlaylistRows
                                                 ? s_music.track_count
                                                 : kPlaylistRows;
            if (s_music.playlist_spacer != nullptr)
                lv_obj_set_height(s_music.playlist_spacer,
                                  static_cast<lv_coord_t>(content_rows * kPlaylistRowStride));
        }

        void musicPageCreate(Page *page)
        {
            if (page == nullptr || page->root == nullptr)
                return;

            s_music.root = page->root;
            portENTER_CRITICAL(&s_playlist_lock);
            s_playback.count = 0U;
            s_playback.index = 0U;
            s_playback.active = true;
            s_playback.order = s_music.order;
            portEXIT_CRITICAL(&s_playlist_lock);
            ui_create_background(page->root);
            lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);
            lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);

            createCover(page->root);

            s_music.song_name = page_ui_label(page->root, "Music Player",
                                              &lv_font_source_han_sans_sc_16_cjk, UI_WIHTE);
            lv_obj_set_style_text_font(s_music.song_name, &lv_font_source_han_sans_sc_16_cjk, LV_PART_MAIN);
            lv_obj_set_style_text_align(s_music.song_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_width(s_music.song_name, kScreenSize);
            lv_obj_align(s_music.song_name, LV_ALIGN_TOP_MID, 0, 336);

            s_music.artist = page_ui_label(page->root, "Artist Name",
                                           &lv_font_source_han_sans_sc_16_cjk, 0x9ea4d7);
            lv_obj_set_style_text_align(s_music.artist, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_width(s_music.artist, kScreenSize);
            lv_obj_align(s_music.artist, LV_ALIGN_TOP_MID, 0, 368);

            s_music.lyric = page_ui_label(page->root, "Lyrics",
                                          &lv_font_source_han_sans_sc_16_cjk, UI_WIHTE);
            lv_obj_set_style_text_align(s_music.lyric, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_width(s_music.lyric, kScreenSize);
            lv_label_set_long_mode(s_music.lyric, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(s_music.lyric, 22);
            lv_obj_align(s_music.lyric, LV_ALIGN_TOP_MID, 0, 421);
            lv_obj_set_style_text_opa(s_music.lyric, LV_OPA_90, LV_PART_MAIN);
            s_music.lyric_next = page_ui_label(page->root, "Next Lyrics",
                                               &lv_font_source_han_sans_sc_16_cjk, 0x9ea4d7);
            lv_obj_set_style_text_align(s_music.lyric_next, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_width(s_music.lyric_next, kScreenSize);
            lv_label_set_long_mode(s_music.lyric_next, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(s_music.lyric_next, 22);
            lv_obj_align(s_music.lyric_next, LV_ALIGN_TOP_MID, 0, 446);

            s_music.progress = lv_slider_create(page->root);
            lv_obj_set_size(s_music.progress, kProgressWidth, 5);
            lv_obj_align(s_music.progress, LV_ALIGN_TOP_MID, 0, kProgressTop);
            lv_slider_set_range(s_music.progress, 0, 1);
            lv_slider_set_value(s_music.progress, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_music.progress, lv_color_hex(0x34386e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_music.progress, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(s_music.progress, 4, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_music.progress, lv_color_hex(UI_WIHTE), LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(s_music.progress, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(s_music.progress, 4, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(s_music.progress, LV_OPA_TRANSP, LV_PART_KNOB);
            lv_obj_add_event_cb(s_music.progress, progressChanged, LV_EVENT_ALL, nullptr);

            s_music.elapsed = page_ui_label(page->root, "00:00", &lv_font_montserrat_14, 0x8b8fb7);
            lv_obj_align(s_music.elapsed, LV_ALIGN_TOP_MID, -198, kProgressTop + 14);
            s_music.duration = page_ui_label(page->root, "00:00", &lv_font_montserrat_14, 0x8b8fb7);
            lv_obj_align(s_music.duration, LV_ALIGN_TOP_MID, 198, kProgressTop + 14);

            lv_obj_t *controls = lv_obj_create(page->root);
            lv_obj_set_size(controls, 480, 72);
            lv_obj_align(controls, LV_ALIGN_TOP_MID, 0, kControlTop);
            lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(controls, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(controls, 0, LV_PART_MAIN);
            lv_obj_remove_flag(controls, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *order_button = makeControlButton(controls, "SEQ", 24, 48, orderClicked);
            if (order_button != nullptr)
            {
                s_music.order_label = lv_obj_get_child(order_button, 0);
                lv_obj_set_style_text_font(s_music.order_label, &lv_font_montserrat_14, LV_PART_MAIN);
                lv_obj_center(s_music.order_label);
            }
            (void)makeControlButton(controls, LV_SYMBOL_PREV, 120, 48, resetTrack);
            s_music.play_button = makeControlButton(controls, LV_SYMBOL_PLAY, 208, 63, playClicked);
            if (s_music.play_button != nullptr)
            {
                s_music.play_icon = lv_obj_get_child(s_music.play_button, 0);
                setTextColor(s_music.play_icon, UI_WIHTE);
            }
            (void)makeControlButton(controls, LV_SYMBOL_NEXT, 312, 48, nextTrack);
            (void)makeControlButton(controls, LV_SYMBOL_LIST, 408, 48, togglePlaylist);

            // Use the complete round display as the arc's coordinate system.
            // The 70..110 degree segment, rotated by 270 degrees, sits on the
            // right side of the 720 px circular screen.
            s_music.volume = lv_arc_create(page->root);
            lv_obj_set_size(s_music.volume, kScreenSize, kScreenSize);
            lv_obj_center(s_music.volume);
            // Restrict pointer hit testing to the circular track so the arc
            // does not intercept taps on the cover or playback controls.
            lv_obj_add_flag(s_music.volume, LV_OBJ_FLAG_ADV_HITTEST);
            lv_arc_set_range(s_music.volume, 0, 100);
            lv_arc_set_bg_angles(s_music.volume, 70, 110);
            lv_arc_set_rotation(s_music.volume, 270);
            // After rotation the start is above the end: reverse makes up louder.
            lv_arc_set_mode(s_music.volume, LV_ARC_MODE_REVERSE);
            std::uint8_t volume = 72U;
            (void)settings_control_get_audio_volume(&volume);
            lv_arc_set_value(s_music.volume, volume);
            lv_obj_set_style_bg_opa(s_music.volume, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_music.volume, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(s_music.volume, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_music.volume, 20, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(s_music.volume, 0, LV_PART_MAIN);
            lv_obj_set_style_arc_color(s_music.volume, lv_color_hex(UI_WHITE), LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_music.volume, 12, LV_PART_MAIN);
            lv_obj_set_style_arc_rounded(s_music.volume, true, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(s_music.volume, LV_OPA_30, LV_PART_MAIN);
            lv_obj_set_style_arc_color(s_music.volume, lv_color_hex(UI_WHITE), LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(s_music.volume, 12, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(s_music.volume, true, LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(s_music.volume, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_music.volume, lv_color_hex(UI_WHITE), LV_PART_KNOB);
            lv_obj_set_style_bg_grad_dir(s_music.volume, LV_GRAD_DIR_NONE, LV_PART_KNOB);
            lv_obj_set_style_bg_opa(s_music.volume, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(s_music.volume, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_pad_all(s_music.volume, 3, LV_PART_KNOB);
            // Position the icon centers on the track's circle, extending its
            // +/-20 degree endpoints by 8 degrees to leave room for the knob.
            constexpr lv_coord_t volume_radius = kScreenSize / 2 - 20 - 12 / 2;
            constexpr int volume_icon_angle = 28;
            const lv_coord_t volume_icon_x = volume_radius * lv_trigo_cos(volume_icon_angle) / (1 << LV_TRIGO_SHIFT);
            const lv_coord_t volume_icon_y = volume_radius * lv_trigo_sin(volume_icon_angle) / (1 << LV_TRIGO_SHIFT);
            s_music.volume_icon = page_ui_label(page->root, LV_SYMBOL_VOLUME_MAX,
                                                 &lv_font_montserrat_16, UI_WHITE);
            lv_obj_align(s_music.volume_icon, LV_ALIGN_CENTER, volume_icon_x, -volume_icon_y);
            s_music.volume_min_icon = page_ui_label(page->root, LV_SYMBOL_MUTE,
                                                     &lv_font_montserrat_16, UI_WHITE);
            lv_obj_align(s_music.volume_min_icon, LV_ALIGN_CENTER, volume_icon_x, volume_icon_y);
            lv_obj_add_event_cb(s_music.volume, volumeChanged, LV_EVENT_ALL, nullptr);

            updateTrackLabels();
            createPlaylist(page->root);
            refreshPlaylist();
            s_music.timer = lv_timer_create(refreshPlayback, 250, nullptr);
        }

        void musicPageDestroy(Page *)
        {
            portENTER_CRITICAL(&s_playlist_lock);
            s_playback.active = false;
            portEXIT_CRITICAL(&s_playlist_lock);
            if (s_music.timer != nullptr)
                lv_timer_del(s_music.timer);
            if (s_music.loader != nullptr)
            {
                // Never delete a running task in the middle of filesystem I/O.
                // Its private context survives until it closes files and exits.
                s_music.loader->cancelled.store(true);
                releaseLoader(s_music.loader);
            }
            releaseCover();
            // Release the PSRAM track cache and lyrics, then restore defaults
            // directly in static storage without a temporary UI state object.
            s_music.~MusicUi();
            ::new (static_cast<void *>(&s_music)) MusicUi{};
        }

        void musicPageLeave(Page *)
        {
            portENTER_CRITICAL(&s_playlist_lock);
            s_playback.active = false;
            portEXIT_CRITICAL(&s_playlist_lock);
            if (bsp::t_panel_p4_round::Board *board = musicBoard(); board != nullptr)
            {
                (void)board->stopMusic();
            }
            (void)ui_page_destroy(PAGE_MUSIC);
        }

        void registerMusicPage()
        {
            // Construct the BSP player at startup, not while opening the page.
            // The callback is inert whenever the protected queue is inactive.
            if (bsp::t_panel_p4_round::Board *board = musicBoard(); board != nullptr)
                (void)board->setMusicNextTrackCallback(musicNextTrack, nullptr);
            Page page{};
            page.id = PAGE_MUSIC;
            page.name = "music";
            page.on_create = musicPageCreate;
            page.on_destroy = musicPageDestroy;
            page.on_leave = musicPageLeave;
            page.on_gesture = page_ui_back_gesture;
            (void)ui_page_register(&page);
        }
    } // namespace
} // namespace factory::ui

extern "C" void music_page_register(void) { factory::ui::registerMusicPage(); }
