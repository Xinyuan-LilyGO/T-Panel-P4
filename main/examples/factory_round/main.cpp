#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <lvgl.h>
#include <bsp/board.hpp>
#include <bsp/pins.hpp>
#include <ui/core/lvgl_port.hpp>
#include <ui/pages/page_registry.hpp>
#include <lvgl_page_manager.h>
#include <esp32c5_connectivity/esp32c5_connectivity.hpp>
#include <services/service_context.hpp>

namespace
{
    constexpr char kTag[] = "factory_round";

    struct Application
    {
        bsp::t_panel_p4_round::Board board;
        connectivity::Esp32c5Connectivity esp32c5;
        connectivity::Esp32c5ConnectivityConfig esp32c5_config{};
        connectivity::Esp32c5WifiStationConfig wifi_config{};
        bool wifi_config_valid{};
        esp_err_t esp32c5_result{ESP_ERR_INVALID_STATE};
        esp_err_t sd_card_result{ESP_ERR_INVALID_STATE};
        esp_err_t audio_result{ESP_ERR_INVALID_STATE};
    } application;

    void initializeConnectivity(Application &app)
    {
        app.esp32c5_config = {};
        app.esp32c5_result = app.esp32c5.initialize(app.esp32c5_config);
        if (app.esp32c5_result == ESP_OK)
        {
            connectivity::Esp32c5CoprocessorInfo info{};
            if (app.esp32c5.coprocessorInfo(info) == ESP_OK)
            {
                ESP_LOGI(
                    kTag,
                    "ESP32-C5 firmware: %lu.%lu.%lu, project=%s",
                    static_cast<unsigned long>(info.firmware.major),
                    static_cast<unsigned long>(info.firmware.minor),
                    static_cast<unsigned long>(info.firmware.patch),
                    info.project_name.data());
            }

            const esp_err_t wifi_config_result =
                connectivity::Esp32c5Connectivity::makeWifiStationConfig(
                    "LilyGo-AABB", "xinyuandianzi", app.wifi_config);
            if (wifi_config_result == ESP_OK)
            {
                app.wifi_config.automatic_reconnect = true;
                app.wifi_config.maximum_reconnect_attempts = 3;
                app.wifi_config_valid = true;
                const esp_err_t wifi_result =
                    app.esp32c5.startWifiStation(app.wifi_config);
                ESP_LOGI(kTag, "Wi-Fi station start: %s", esp_err_to_name(wifi_result));
            }
            else
            {
                ESP_LOGE(kTag, "Wi-Fi configuration failed: %s",
                         esp_err_to_name(wifi_config_result));
            }
        }
        ESP_LOGI(
            kTag,
            "ESP32-C5 SDIO initialization: %s",
            esp_err_to_name(app.esp32c5_result));

        // Publish the context only after initialize() has finished. Wi-Fi
        // controls must never call esp_wifi_* while ESP-Hosted is starting.
        factory::services::set_context(
            app.board,
            app.esp32c5,
            &app.wifi_config,
            &app.wifi_config_valid,
            &app.esp32c5_result);
    }
}

void logHeapSnapshot(const char *stage)
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

    ESP_LOGI(kTag, "heap snapshot: %s", stage);
    for (const HeapType &heap : heap_types)
    {
        const std::uint32_t total = heap_caps_get_total_size(heap.capabilities);
        const std::uint32_t free = heap_caps_get_free_size(heap.capabilities);
        const std::uint32_t used = total > free ? total - free : 0;
        // Print tenths of a percent using integer arithmetic; this avoids
        // requiring printf floating-point support in ESP-IDF.
        const std::uint32_t usage_tenths = total != 0
                                               ? static_cast<std::uint32_t>(
                                                     (static_cast<std::uint64_t>(used) * 1000U) / total)
                                               : 0;
        ESP_LOGI(kTag,
                 "  %-8s total=%u, free=%u, used=%u, usage=%u.%u%%, largest=%u, minimum=%u",
                 heap.name,
                 total,
                 free,
                 used,
                 usage_tenths / 10U,
                 usage_tenths % 10U,
                 heap_caps_get_largest_free_block(heap.capabilities),
                 heap_caps_get_minimum_free_size(heap.capabilities));
    }
}

void heapMonitorTask(void *argument)
{
    (void)argument;
    for (;;)
    {
        logHeapSnapshot("runtime (every 5s)");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

extern "C" void app_main()
{
    logHeapSnapshot("before board initialize");
    ESP_ERROR_CHECK(application.board.initializePower());
    logHeapSnapshot("after board initialize");

    // Music playback reads tracks from the mounted SD card and sends decoded
    // PCM through the ES8389 codec. Keep startup resilient when either device
    // is absent so the rest of the factory UI remains usable.
    bsp::t_panel_p4_round::SdCardConfig sd_config{};
    application.sd_card_result = application.board.initializeSdCard(sd_config);
    ESP_LOGI(kTag, "SD card initialization: %s", esp_err_to_name(application.sd_card_result));

    bsp::t_panel_p4_round::AudioConfig audio_config{};
    audio_config.output_volume_db = 0.0F;  
    audio_config.microphone_gain = bsp::t_panel_p4_round::AudioMicrophoneGain::db24_5;
    audio_config.enable_capture = true;
    audio_config.enable_speaker = true;
    audio_config.no_dac_reference = true;
    audio_config.sample_rate_hz = 48000;
    application.audio_result = application.board.initializeAudio(audio_config);
    if (application.audio_result == ESP_OK)
        application.audio_result = application.board.setAudioCaptureEnabled(false);
    ESP_LOGI(kTag, "audio ES8389 initialization: %s", esp_err_to_name(application.audio_result));

    bsp::t_panel_p4_round::DisplayConfig display_config{};
    display_config.frame_buffer_count = 2;
    initializeConnectivity(application);

    ESP_ERROR_CHECK(application.board.initializeDisplay(display_config));
    ESP_ERROR_CHECK(application.board.initializeTouch());
    ESP_ERROR_CHECK(application.board.setBacklightBrightness(100));
    logHeapSnapshot("after display and touch initialize");

    factory::ui::setHomeBoard(&application.board);
    factory::ui::setCameraBoard(&application.board);
    factory::ui::setHomeConnectivity(&application.esp32c5);
    ESP_ERROR_CHECK(factory::ui::initializeLvgl(application.board));

    // Use the same page-manager lifecycle as factory_standard. The manager
    // starts on its built-in start page, then switches to the Home grid page
    // through its FreeRTOS message queue.
    home_page_register();
    music_page_register();
    record_page_register();
    camera_page_register();
    file_page_register();
    lora_page_register();
    settings_page_register();
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(ui_page_switch_async(PAGE_HOME));

    xTaskCreate(
        heapMonitorTask,
        "factory_round_heap",
        3072,
        nullptr,
        1,
        nullptr);
}
