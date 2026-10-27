#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>

#include <bsp/t_panel_p4_standard.hpp>

namespace
{
constexpr char kTag[] = "sd_example";
// Keep the name compatible with FAT 8.3 when long filename support is disabled.
constexpr char kFileName[] = "sdtest.txt";
constexpr char kTestText[] =
    "T-Panel-P4 SD card read/write test\n"
    "The BSP mounted this card successfully.\n";

esp_err_t writeTestFile(const char *path)
{
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        ESP_LOGE(kTag, "open %s for writing failed: %s", path, std::strerror(errno));
        return ESP_FAIL;
    }

    constexpr std::size_t expected_size = sizeof(kTestText) - 1U;
    const std::size_t written = std::fwrite(kTestText, 1, expected_size, file);
    const int close_result = std::fclose(file);
    if (written != expected_size || close_result != 0)
    {
        ESP_LOGE(
            kTag,
            "write %s failed: wrote %u of %u bytes",
            path,
            static_cast<unsigned int>(written),
            static_cast<unsigned int>(expected_size));
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "wrote %u bytes to %s", static_cast<unsigned int>(written), path);
    return ESP_OK;
}

esp_err_t readAndVerifyTestFile(const char *path)
{
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        ESP_LOGE(kTag, "open %s for reading failed: %s", path, std::strerror(errno));
        return ESP_FAIL;
    }

    std::array<char, sizeof(kTestText)> buffer{};
    const std::size_t bytes_read = std::fread(buffer.data(), 1, buffer.size(), file);
    const bool read_failed = std::ferror(file) != 0;
    const int close_result = std::fclose(file);
    constexpr std::size_t expected_size = sizeof(kTestText) - 1U;

    if (read_failed || close_result != 0)
    {
        ESP_LOGE(kTag, "read %s failed", path);
        return ESP_FAIL;
    }
    if (bytes_read != expected_size ||
        std::memcmp(buffer.data(), kTestText, expected_size) != 0)
    {
        ESP_LOGE(
            kTag,
            "verify %s failed: read %u bytes, expected %u",
            path,
            static_cast<unsigned int>(bytes_read),
            static_cast<unsigned int>(expected_size));
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(kTag, "read and verified %u bytes from %s", static_cast<unsigned int>(bytes_read), path);
    ESP_LOGI(kTag, "file contents:\n%s", buffer.data());
    return ESP_OK;
}

esp_err_t runExample()
{
    bsp::t_panel_p4_standard::Board board;
    esp_err_t result = board.initialize();
    if (result != ESP_OK)
        return result;

    bsp::t_panel_p4_standard::SdCardConfig config{};
    config.mount_point = "/sdcard";
    config.format_if_mount_failed = false;

    result = board.initializeSdCard(config);
    if (result == ESP_OK)
    {
        std::array<char, 96> path{};
        const int path_length = std::snprintf(
            path.data(),
            path.size(),
            "%s/%s",
            board.sdCardMountPoint(),
            kFileName);
        if (path_length < 0 || static_cast<std::size_t>(path_length) >= path.size())
        {
            ESP_LOGE(kTag, "test file path is too long");
            result = ESP_ERR_INVALID_SIZE;
        }
        else
        {
            result = writeTestFile(path.data());
            if (result == ESP_OK)
                result = readAndVerifyTestFile(path.data());
        }
    }

    const esp_err_t cleanup_result = board.deinitialize();
    if (result == ESP_OK)
        result = cleanup_result;
    return result;
}
}

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "starting SD card read/write example");
    const esp_err_t error = runExample();
    if (error != ESP_OK)
    {
        ESP_LOGE(kTag, "SD card read/write example failed: %s", esp_err_to_name(error));
        return;
    }
    ESP_LOGI(kTag, "SD card read/write example passed");
}
