#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <string>

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_STANDARD
#include <bsp/t_panel_p4_standard.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE T_PANEL_P4_FPS_BOARD_NAMESPACE
#elif T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_ROUND
#include <bsp/t_panel_p4_round.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE t_panel_p4_round
#elif T_PANEL_P4_HARDWARE_VERSION == T_PANEL_P4_HARDWARE_VERSION_RECT
#include <bsp/t_panel_p4_rect.hpp>
#define T_PANEL_P4_FPS_BOARD_NAMESPACE t_panel_p4_rect
#else
#error "Unsupported T_PANEL_P4_HARDWARE_VERSION"
#endif

namespace
{
using namespace bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE;

constexpr char kTag[] = "play_sd_mp3";
constexpr char kMusicDirectory[] = "/sdcard/music";
constexpr std::size_t kMaxPathLength = 256U;
constexpr gpio_num_t kPreviousButton = pins::button;
constexpr gpio_num_t kNextButton = pins::boot_button;

struct Track
{
    std::array<char, kMaxPathLength> name{};
    std::array<char, kMaxPathLength + 16U> path{};
};

bool hasMp3Extension(const char *name) noexcept
{
    if (name == nullptr)
        return false;
    const std::size_t length = std::strlen(name);
    constexpr char suffix[] = ".mp3";
    if (length < sizeof(suffix) - 1U)
        return false;
    for (std::size_t i = 0; i < sizeof(suffix) - 1U; ++i)
    {
        if (std::tolower(static_cast<unsigned char>(name[length - 4U + i])) != suffix[i])
            return false;
    }
    return true;
}

int pollTrackButton() noexcept
{
    static bool previous_released = true;
    static bool next_released = true;
    const bool previous_pressed = gpio_get_level(kPreviousButton) == 0;
    const bool next_pressed = gpio_get_level(kNextButton) == 0;
    int action = 0;
    if (previous_pressed && previous_released)
        action = -1;
    else if (next_pressed && next_released)
        action = 1;
    previous_released = !previous_pressed;
    next_released = !next_pressed;
    return action;
}

void musicEventCallback(const MusicEvent &event, void *)
{
    if (event.type == MusicEventType::music_info)
    {
        ESP_LOGI(kTag, "format: %d Hz, %u ch, %u bit, %d bps",
                 event.format.sample_rate_hz,
                 static_cast<unsigned>(event.format.channels),
                 static_cast<unsigned>(event.format.bits_per_sample),
                 event.format.bitrate_bps);
    }
    else if (event.type == MusicEventType::error)
    {
        ESP_LOGE(kTag, "music error: %s", esp_err_to_name(event.error));
    }
}

esp_err_t scanTracks(std::array<Track, 64U> &tracks, std::size_t &count)
{
    DIR *directory = opendir(kMusicDirectory);
    if (directory == nullptr)
        return ESP_ERR_NOT_FOUND;
    count = 0U;
    while (dirent *entry = readdir(directory))
    {
        if (entry->d_type == DT_DIR || !hasMp3Extension(entry->d_name))
            continue;
        if (count == tracks.size())
            break;
        const char *entry_name = entry->d_name;
        std::strncpy(tracks[count].name.data(), entry_name, kMaxPathLength - 1U);
        std::snprintf(tracks[count].path.data(), tracks[count].path.size(),
                      "%s/%s", kMusicDirectory, entry_name);
        ++count;
    }
    closedir(directory);
    std::sort(tracks.begin(), tracks.begin() + count,
              [](const Track &a, const Track &b) {
                  return std::strcmp(a.name.data(), b.name.data()) < 0;
              });
    return count == 0U ? ESP_ERR_NOT_FOUND : ESP_OK;
}

esp_err_t playTrack(Board &board, const Track &track, std::size_t index, std::size_t count, int &action)
{
    MusicPlaybackConfig config{};
    config.auto_configure_audio = false;
    config.allow_resample = true;
    config.reject_unknown_format = true;

    std::string lyrics;
    const esp_err_t lyrics_error = board.getMusicLyrics(track.path.data(), lyrics);
    if (lyrics_error == ESP_OK && !lyrics.empty())
        ESP_LOGI(kTag, "lyrics available for %s (%u bytes)",
                 track.name.data(), static_cast<unsigned>(lyrics.size()));
    else if (lyrics_error != ESP_ERR_NOT_FOUND)
        ESP_LOGW(kTag, "lyrics read failed for %s: %s",
                 track.name.data(), esp_err_to_name(lyrics_error));

    ESP_LOGI(kTag, "playing %u/%u: %s",
             static_cast<unsigned>(index + 1U), static_cast<unsigned>(count), track.name.data());
    esp_err_t error = board.playMusicFile(track.path.data(), config);
    if (error != ESP_OK)
        return error;

    action = 0;
    while (true)
    {
        const MusicState state = board.musicState();
        if (state == MusicState::finished)
            return ESP_OK;
        if (state == MusicState::error || state == MusicState::stopped)
            return board.musicLastError() == ESP_OK ? ESP_FAIL : board.musicLastError();

        action = pollTrackButton();
        if (action != 0)
        {
            ESP_LOGI(kTag, "%s button: switching track",
                     action < 0 ? "previous" : "next");
            return board.stopMusic(2000U);
        }

        ESP_LOGI(kTag, "%s: %lu/%lu ms",
                 track.name.data(),
                 static_cast<unsigned long>(board.musicPositionMs()),
                 static_cast<unsigned long>(board.musicDurationMs()));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
}

extern "C" void app_main(void)
{
    static Board board;
    ESP_ERROR_CHECK(board.initialize());

    gpio_config_t button_config{};
    button_config.pin_bit_mask = (1ULL << kPreviousButton) | (1ULL << kNextButton);
    button_config.mode = GPIO_MODE_INPUT;
    button_config.pull_up_en = GPIO_PULLUP_ENABLE;
    button_config.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&button_config));

    SdCardConfig sd_config{};
    sd_config.mount_point = "/sdcard";
    sd_config.format_if_mount_failed = false;
    ESP_ERROR_CHECK(board.initializeSdCard(sd_config));

    AudioConfig audio_config{};
    audio_config.codec = AudioCodecChip::es8389;
    audio_config.sample_rate_hz = 48000U;
    audio_config.word_length = AudioWordLength::bits16;
    audio_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    audio_config.output_volume_db = -12.0F;
    audio_config.enable_capture = false;
    audio_config.enable_speaker = true;
    audio_config.no_dac_reference = true;
    ESP_ERROR_CHECK(board.initializeAudio(audio_config));
    ESP_ERROR_CHECK(board.setMusicEventCallback(musicEventCallback));

    static std::array<Track, 64U> tracks{};
    std::size_t count = 0U;
    ESP_ERROR_CHECK(scanTracks(tracks, count));
    for (std::size_t i = 0U; i < count; ++i)
        ESP_LOGI(kTag, "music[%u]: %s", static_cast<unsigned>(i), tracks[i].name.data());

    std::size_t index = 0U;
    while (true)
    {
        int action = 0;
        const esp_err_t error = playTrack(board, tracks[index], index, count, action);
        if (error != ESP_OK && action == 0)
        {
            ESP_LOGE(kTag, "track failed: %s", esp_err_to_name(error));
            break;
        }
        if (action < 0)
            index = index == 0U ? count - 1U : index - 1U;
        else
            index = (index + 1U) % count;
    }
}
