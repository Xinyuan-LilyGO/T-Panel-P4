#include <array>
#include <cstddef>
#include <cstdint>

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <pdl/core/status.hpp>

namespace
{
    constexpr char kTag[] = "audio_loopback";
    constexpr std::uint32_t kSampleRateHz = 48000;
    constexpr float kOutputVolumeDb = 0.0F;
    constexpr std::size_t kStereoSampleCount = 1024;
    constexpr std::size_t kStereoFrameBytes = 2U * sizeof(std::int16_t);
    constexpr std::uint32_t kIoTimeoutMs = 1000;
    constexpr TickType_t kErrorDelay = pdMS_TO_TICKS(20);
    constexpr TickType_t kMeterInterval = pdMS_TO_TICKS(1000);
    std::array<std::int16_t, kStereoSampleCount> samples{};

    std::uint32_t magnitude(std::int16_t sample) noexcept
    {
        const std::int32_t value = sample;
        return static_cast<std::uint32_t>(value < 0 ? -value : value);
    }

    void updatePeaks(
        const std::int16_t *samples,
        std::size_t sample_count,
        std::uint32_t &microphone_1_peak,
        std::uint32_t &microphone_2_peak)
    {
        for (std::size_t index = 0; index + 1U < sample_count; index += 2U)
        {
            const std::uint32_t left = magnitude(samples[index]);
            const std::uint32_t right = magnitude(samples[index + 1U]);
            if (left > microphone_1_peak)
                microphone_1_peak = left;
            if (right > microphone_2_peak)
                microphone_2_peak = right;
        }
    }

    esp_err_t writeAll(
        bsp::t_panel_p4_standard::Board &board,
        const std::uint8_t *data,
        std::size_t size)
    {
        std::size_t offset{};
        while (offset < size)
        {
            std::size_t bytes_written{};
            const esp_err_t error = board.writeAudio(
                data + offset,
                size - offset,
                bytes_written,
                kIoTimeoutMs);
            if (error != ESP_OK)
                return error;
            if (bytes_written == 0U)
                return ESP_ERR_TIMEOUT;
            offset += bytes_written;
        }
        return ESP_OK;
    }

    esp_err_t runLoopback()
    {
        bsp::t_panel_p4_standard::Board board;
        esp_err_t error = board.initialize();
        if (error != ESP_OK)
            return error;

        bsp::t_panel_p4_standard::AudioConfig audio_config{};
        audio_config.sample_rate_hz = kSampleRateHz;
        audio_config.word_length = bsp::t_panel_p4_standard::AudioWordLength::bits16;
        audio_config.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        audio_config.output_volume_db = kOutputVolumeDb;
        audio_config.microphone_gain = bsp::t_panel_p4_standard::AudioMicrophoneGain::db12_5;
        audio_config.no_dac_reference = true;
        audio_config.enable_speaker = true;
        error = board.initializeAudio(audio_config);
        if (error != ESP_OK)
            return error;

        ESP_LOGI(
            kTag,
            "two-microphone loopback started: %lu Hz, 16-bit stereo, volume=%.1f dB",
            static_cast<unsigned long>(kSampleRateHz),
            static_cast<double>(kOutputVolumeDb));

        std::uint32_t microphone_1_peak{};
        std::uint32_t microphone_2_peak{};
        TickType_t last_meter_time = xTaskGetTickCount();

        while (true)
        {
            std::size_t bytes_read{};
            error = board.readAudio(
                samples.data(),
                sizeof(samples),
                bytes_read,
                kIoTimeoutMs);
            if (error != ESP_OK)
            {
                ESP_LOGE(kTag, "I2S read failed: %s", esp_err_to_name(error));
                vTaskDelay(kErrorDelay);
                continue;
            }

            bytes_read -= bytes_read % kStereoFrameBytes;
            if (bytes_read == 0U)
                continue;

            updatePeaks(
                samples.data(),
                bytes_read / sizeof(samples[0]),
                microphone_1_peak,
                microphone_2_peak);

            error = writeAll(
                board,
                reinterpret_cast<const std::uint8_t *>(samples.data()),
                bytes_read);
            if (error != ESP_OK)
            {
                ESP_LOGE(kTag, "I2S write failed: %s", esp_err_to_name(error));
                vTaskDelay(kErrorDelay);
            }

            const TickType_t now = xTaskGetTickCount();
            if (now - last_meter_time >= kMeterInterval)
            {
                ESP_LOGI(
                    kTag,
                    "microphone peaks: MIC1=%lu, MIC2=%lu",
                    static_cast<unsigned long>(microphone_1_peak),
                    static_cast<unsigned long>(microphone_2_peak));
                microphone_1_peak = 0;
                microphone_2_peak = 0;
                last_meter_time = now;
            }
        }
    }
}

extern "C" void app_main(void)
{
    const esp_err_t error = runLoopback();
    if (error != ESP_OK)
    {
        ESP_LOGE(kTag, "audio loopback stopped: %s", esp_err_to_name(error));
    }
}
