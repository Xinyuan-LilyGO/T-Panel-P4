#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

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

extern const std::uint8_t wav_start[] asm("_binary_sample_3s_wav_start");
extern const std::uint8_t wav_end[] asm("_binary_sample_3s_wav_end");

using namespace bsp::T_PANEL_P4_FPS_BOARD_NAMESPACE;
Board board;

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(board.initialize());

    AudioConfig config{};
    config.codec = AudioCodecChip::es8389;
    config.sample_rate_hz = 48000;
    config.word_length = AudioWordLength::bits16;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    config.slot_mode = I2S_SLOT_MODE_STEREO;
    config.slot_mask = I2S_STD_SLOT_BOTH;

    config.output_volume_db = -12.0F;
    config.microphone_gain = AudioMicrophoneGain::db12_5;

    config.enable_capture = true;
    config.enable_speaker = true;
    config.no_dac_reference = false;

    ESP_ERROR_CHECK(board.initializeAudio(config));
    std::size_t written = 0;
    while (true)
    {
        ESP_LOGI("audio", "playing WAV file...");
        const std::size_t wav_size = static_cast<std::size_t>(wav_end - wav_start);
        const std::size_t bytes_written = board.writeAudio(wav_start, wav_size, written, pdMS_TO_TICKS(5000));
        if (bytes_written != wav_size)
        {
            ESP_LOGE("audio", "failed to write all audio data: %zu of %zu bytes written", bytes_written, wav_size);
        }
        else
        {
            ESP_LOGI("audio", "WAV file playback complete");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
