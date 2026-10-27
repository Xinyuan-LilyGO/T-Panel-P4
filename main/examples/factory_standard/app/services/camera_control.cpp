#include "camera_control.h"
#include "service_context.hpp"

#include <bsp/t_panel_p4_standard.hpp>
#include <esp_err.h>
#include <esp_log.h>
#include <driver/jpeg_encode.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <freertos/task.h>

namespace
{
constexpr char kTag[] = "factory_camera";
#define s_board (factory::services::board())
esp_err_t s_last_error{ESP_OK};
volatile bool s_capture_busy{};
volatile int s_capture_result{};
char s_capture_path[192]{};
}

extern "C" bool camera_control_enter(void)
{
    if (s_board == nullptr)
    {
        s_last_error = ESP_ERR_INVALID_STATE;
        return false;
    }

    s_last_error = s_board->isCameraInitialized()
                       ? s_board->startCameraStreaming()
                       : s_board->initializeCamera();
    if (s_last_error != ESP_OK)
        ESP_LOGE(kTag, "camera page start failed: %s", esp_err_to_name(s_last_error));
    return s_last_error == ESP_OK;
}

extern "C" bool camera_control_leave(void)
{
    if (s_board == nullptr)
    {
        s_last_error = ESP_ERR_INVALID_STATE;
        return false;
    }
    if (!s_board->isCameraInitialized())
    {
        s_last_error = ESP_OK;
        return true;
    }

    s_last_error = s_board->deinitializeCamera();
    if (s_last_error != ESP_OK)
        ESP_LOGE(kTag, "camera page deinitialize failed: %s", esp_err_to_name(s_last_error));
    return s_last_error == ESP_OK;
}

extern "C" const char *camera_control_last_error_name(void)
{
    return esp_err_to_name(s_last_error);
}

extern "C" bool camera_control_read_frame_rgb565(uint16_t *destination,
                                                  size_t destination_pixels,
                                                  uint16_t destination_width,
                                                  uint16_t destination_height)
{
    if (s_board == nullptr || destination == nullptr || destination_width == 0U ||
        destination_height == 0U || destination_pixels <
        static_cast<size_t>(destination_width) * destination_height)
        return false;

    bsp::t_panel_p4_standard::CameraFrame frame{};
    if (s_board->acquireCameraFrame(frame, 120U) != ESP_OK || frame.data == nullptr)
        return false;
    const auto mode = s_board->cameraModeInfo();
    const uint32_t source_width = mode.width;
    const uint32_t source_height = mode.height;
    const size_t source_bytes = static_cast<size_t>(source_width) * source_height * 2U;
    if (source_width == 0U || source_height == 0U || frame.size < source_bytes)
    {
        (void)s_board->releaseCameraFrame(frame.data);
        return false;
    }

    const auto *source = reinterpret_cast<const uint16_t *>(frame.data);
    for (uint32_t y = 0U; y < destination_height; ++y)
    {
        const uint32_t source_y = y * source_height / destination_height;
        for (uint32_t x = 0U; x < destination_width; ++x)
        {
            const uint32_t source_x = x * source_width / destination_width;
            destination[y * destination_width + x] =
                source[source_y * source_width + source_x];
        }
    }
    (void)s_board->releaseCameraFrame(frame.data);
    return true;
}

extern "C" bool camera_control_get_mode(uint16_t *width, uint16_t *height,
                                        uint8_t *frames_per_second)
{
    if (s_board == nullptr || width == nullptr || height == nullptr ||
        frames_per_second == nullptr || !s_board->isCameraInitialized())
        return false;
    const auto mode = s_board->cameraModeInfo();
    *width = mode.width;
    *height = mode.height;
    *frames_per_second = mode.frames_per_second;
    return *width != 0U && *height != 0U;
}

static bool capture_photo_sync(char *path, size_t path_size)
{
    if (s_board == nullptr || path == nullptr || path_size == 0U ||
        !s_board->isCameraStreaming())
    {
        ESP_LOGW(kTag, "capture rejected board=%p streaming=%d", s_board,
                 s_board != nullptr && s_board->isCameraStreaming() ? 1 : 0);
        return false;
    }
    (void)mkdir("/sdcard/camera", 0777);

    bsp::t_panel_p4_standard::CameraFrame frame{};
    if (s_board->acquireCameraFrame(frame, 2000U) != ESP_OK || frame.data == nullptr)
    {
        ESP_LOGW(kTag, "capture frame acquire failed");
        return false;
    }
    const auto mode = s_board->cameraModeInfo();
    const uint32_t width = mode.width;
    const uint32_t height = mode.height;
    bool saved = false;
    bool candidate_created = false;
    uint32_t encoded_size = 0U;
    FILE *file = nullptr;
    char candidate[192]{};
    for (unsigned index = 0U; index < 1000U; ++index)
    {
        std::snprintf(candidate, sizeof(candidate), "/sdcard/camera/PHOTO_%03u.JPG", index);
        file = std::fopen(candidate, "rb");
        if (file != nullptr)
        {
            std::fclose(file);
            file = nullptr;
            continue;
        }
        file = std::fopen(candidate, "wb");
        candidate_created = file != nullptr;
        break;
    }
    if (file != nullptr && width != 0U && height != 0U &&
        frame.size >= static_cast<size_t>(width) * height * 2U)
    {
        jpeg_encode_engine_cfg_t engine_cfg = {.intr_priority = 0, .timeout_ms = 5000};
        jpeg_encode_cfg_t encode_cfg = {
            .height = height, .width = width,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
            .image_quality = 85,
        };
        jpeg_encode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER};
        jpeg_encode_memory_alloc_cfg_t input_mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER};
        size_t output_size = 0U;
        size_t input_size = 0U;
        uint8_t *output = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(
            static_cast<size_t>(width) * height * 3U, &mem_cfg, &output_size));
        uint8_t *input = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(
            static_cast<size_t>(width) * height * 2U, &input_mem_cfg, &input_size));
        jpeg_encoder_handle_t encoder = nullptr;
        if (input != nullptr)
        {
            /* The CSI/ISP RGB565 byte stream is already in the format expected
             * by the JPEG encoder. Keep the original byte order unchanged. */
            const size_t frame_bytes = static_cast<size_t>(width) * height * 2U;
            std::memcpy(input, frame.data, frame_bytes);
        }
        if (input != nullptr && output != nullptr &&
            jpeg_new_encoder_engine(&engine_cfg, &encoder) == ESP_OK &&
            jpeg_encoder_process(encoder, &encode_cfg,
                                 input, static_cast<uint32_t>(input_size), output,
                                 static_cast<uint32_t>(output_size), &encoded_size) == ESP_OK)
        {
            saved = std::fwrite(output, 1, encoded_size, file) == encoded_size &&
                    std::ferror(file) == 0;
            std::fflush(file);
            if (saved)
                saved = fsync(fileno(file)) == 0;
        }
        if (file != nullptr)
            std::fclose(file);
        if (encoder != nullptr)
            (void)jpeg_del_encoder_engine(encoder);
        std::free(input);
        std::free(output);
    }
    else if (file != nullptr)
        std::fclose(file);
    if (file == nullptr)
        ESP_LOGW(kTag, "capture file open failed path=%s errno=%d", candidate, errno);
    (void)s_board->releaseCameraFrame(frame.data);
    if (!saved)
    {
        ESP_LOGW(kTag, "capture save failed path=%s errno=%d", candidate, errno);
        if (candidate_created && candidate[0] != '\0')
            std::remove(candidate);
        return false;
    }
    ESP_LOGI(kTag, "capture saved path=%s bytes=%u", candidate,
             static_cast<unsigned>(encoded_size));
    std::snprintf(path, path_size, "%s", candidate);
    return true;
}

static void camera_capture_task(void *)
{
    char path[192]{};
    const bool ok = capture_photo_sync(path, sizeof(path));
    if (ok)
        std::snprintf(s_capture_path, sizeof(s_capture_path), "%s", path);
    s_capture_result = ok ? 1 : -1;
    s_capture_busy = false;
    vTaskDelete(nullptr);
}

extern "C" bool camera_control_capture_photo(char *path, size_t path_size)
{
    if (s_capture_busy || s_board == nullptr || !s_board->isCameraStreaming())
        return false;
    s_capture_path[0] = '\0';
    s_capture_result = 0;
    s_capture_busy = true;
    if (xTaskCreate(camera_capture_task, "camera_save", 8192, nullptr, 5, nullptr) != pdPASS)
    {
        s_capture_busy = false;
        s_capture_result = -1;
        return false;
    }
    if (path != nullptr && path_size > 0U)
        path[0] = '\0';
    return true;
}

extern "C" int camera_control_capture_status(char *path, size_t path_size)
{
    if (path != nullptr && path_size > 0U)
        std::snprintf(path, path_size, "%s", s_capture_path);
    if (s_capture_busy)
        return 0;
    return s_capture_result;
}

extern "C" bool camera_control_delete_photo(const char *path)
{
    constexpr char kPhotoPrefix[] = "/sdcard/camera/PHOTO_";
    if (path == nullptr || std::strncmp(path, kPhotoPrefix, sizeof(kPhotoPrefix) - 1U) != 0)
        return false;
    /* Only allow a direct PHOTO_*.JPG entry; reject path traversal or subdirectories. */
    const char *name = path + sizeof(kPhotoPrefix) - 1U;
    if (*name == '\0' || std::strchr(name, '/') != nullptr ||
        std::strchr(name, '\\') != nullptr)
        return false;
    const char *extension = std::strrchr(path, '.');
    if (extension == nullptr || (std::strcmp(extension, ".BMP") != 0 &&
                                std::strcmp(extension, ".bmp") != 0 &&
                                std::strcmp(extension, ".JPG") != 0 &&
                                std::strcmp(extension, ".jpg") != 0))
        return false;
    return std::remove(path) == 0;
}
