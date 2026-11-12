#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/board.hpp>
#include "wav.hpp"

extern const std::uint8_t wav_start[] asm("_binary_sample_3s_wav_start");
extern const std::uint8_t wav_end[] asm("_binary_sample_3s_wav_end");

using namespace bsp::T_PANEL_P4_BSP_NAMESPACE;
Board board;

extern "C" void app_main(void)
{
    example::WavPcm pcm{};
    ESP_ERROR_CHECK(example::parseWav(
                        wav_start, static_cast<std::size_t>(wav_end - wav_start), pcm)
                        ? ESP_OK : ESP_ERR_NOT_SUPPORTED);
    AudioConfig config{};
    config.codec = AudioCodecChip::es8389;
    config.sample_rate_hz = pcm.sample_rate_hz;
    config.word_length = AudioWordLength::bits16;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    config.slot_mode = I2S_SLOT_MODE_STEREO;
    config.slot_mask = I2S_STD_SLOT_BOTH;

    config.output_volume_db = -12.0F;
    config.microphone_gain = AudioMicrophoneGain::db12_5;

    config.enable_capture = false;
    config.enable_speaker = true;
    config.no_dac_reference = false;

    ESP_ERROR_CHECK(board.initializeAudio(config));
    ESP_LOGI("audio", "WAV PCM: %lu Hz, 16-bit stereo, %zu bytes",
             static_cast<unsigned long>(pcm.sample_rate_hz), pcm.size);
    while (true)
    {
        ESP_LOGI("audio", "playing WAV file...");
        ESP_ERROR_CHECK(board.setAudioOutputEnabled(true));
        std::size_t offset = 0U;
        esp_err_t result = ESP_OK;
        while (offset < pcm.size)
        {
            std::size_t written = 0U;
            result = board.writeAudio(pcm.data + offset,
                                      std::min<std::size_t>(4096U, pcm.size - offset),
                                      written, 1000U);
            offset += written;
            if (result != ESP_OK || written == 0U)
            {
                if (result == ESP_OK)
                    result = ESP_ERR_TIMEOUT;
                break;
            }
        }
        if (result == ESP_OK)
            result = board.drainAudio();
        ESP_ERROR_CHECK(board.setAudioOutputEnabled(false));
        if (result != ESP_OK)
        {
            ESP_LOGE("audio", "playback failed: %s (%zu/%zu PCM bytes)",
                     esp_err_to_name(result), offset, pcm.size);
        }
        else
        {
            ESP_LOGI("audio", "WAV file playback complete");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
