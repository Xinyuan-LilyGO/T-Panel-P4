#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace example
{
    struct WavPcm
    {
        const std::uint8_t *data{};
        std::size_t size{};
        std::uint32_t sample_rate_hz{};
    };

    // This example supports uncompressed 16-bit stereo WAV files. Walk RIFF
    // chunks instead of assuming a 44-byte header (LIST/JUNK chunks are legal).
    inline bool parseWav(const std::uint8_t *file, std::size_t size, WavPcm &pcm)
    {
        pcm = {};
        if (file == nullptr || size < 12U || std::memcmp(file, "RIFF", 4) != 0 ||
            std::memcmp(file + 8, "WAVE", 4) != 0)
            return false;
        const auto le16 = [](const std::uint8_t *p) -> std::uint16_t {
            return p[0] | (static_cast<std::uint16_t>(p[1]) << 8U);
        };
        const auto le32 = [](const std::uint8_t *p) -> std::uint32_t {
            return p[0] | (static_cast<std::uint32_t>(p[1]) << 8U) |
                   (static_cast<std::uint32_t>(p[2]) << 16U) |
                   (static_cast<std::uint32_t>(p[3]) << 24U);
        };
        const std::size_t riff_size = le32(file + 4);
        if (riff_size < 4U || riff_size > size - 8U)
            return false;
        const std::size_t end = riff_size + 8U;
        WavPcm result{};
        bool has_format = false;
        for (std::size_t offset = 12U; offset < end;)
        {
            if (end - offset < 8U)
                return false;
            const auto *chunk = file + offset;
            const std::size_t chunk_size = le32(chunk + 4);
            offset += 8U;
            if (chunk_size > end - offset)
                return false;
            const auto *data = file + offset;
            if (std::memcmp(chunk, "fmt ", 4) == 0)
            {
                if (has_format || chunk_size < 16U || le16(data) != 1U ||
                    le16(data + 2) != 2U || le16(data + 12) != 4U ||
                    le16(data + 14) != 16U || le32(data + 4) == 0U ||
                    le32(data + 8) != static_cast<std::uint64_t>(le32(data + 4)) * 4U)
                    return false;
                result.sample_rate_hz = le32(data + 4);
                has_format = true;
            }
            else if (std::memcmp(chunk, "data", 4) == 0)
            {
                if (result.data != nullptr || chunk_size == 0U || chunk_size % 4U != 0U)
                    return false;
                result.data = data;
                result.size = chunk_size;
            }
            offset += chunk_size;
            if ((chunk_size & 1U) != 0U)
            {
                if (offset == end)
                    return false;
                ++offset;
            }
        }
        if (!has_format || result.data == nullptr)
            return false;
        pcm = result;
        return true;
    }
}
