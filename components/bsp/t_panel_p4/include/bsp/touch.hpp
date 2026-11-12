#pragma once

#include <bsp/hardware.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

#if T_PANEL_P4_BSP_IS_STANDARD
#include <pdl/drivers/touch/jadard/jadard.hpp>
#elif T_PANEL_P4_BSP_IS_ROUND
#include <pdl/drivers/touch/gt911/gt911.hpp>
#else
#include <pdl/drivers/touch/ili9882/ili9882.hpp>
#endif

namespace bsp::T_PANEL_P4_BSP_NAMESPACE
{
    namespace detail
    {
#if T_PANEL_P4_BSP_IS_STANDARD
        using NativeTouch = pdl::touch::JadardTouch;
        using NativeTouchData = pdl::touch::JadardTouchData;
#elif T_PANEL_P4_BSP_IS_ROUND
        using NativeTouch = pdl::touch::Gt911Touch;
        using NativeTouchData = pdl::touch::Gt911TouchData;
#else
        using NativeTouch = pdl::touch::Ili9882Touch;
        using NativeTouchData = pdl::touch::Ili9882TouchData;
#endif
    }

    struct TouchPoint
    {
        std::uint16_t x{};
        std::uint16_t y{};
        std::uint16_t strength{};
        std::uint8_t track_id{};
    };

    struct TouchData
    {
        static constexpr std::size_t maximumPoints = 10;
        std::uint8_t finger_count{};
        bool edge_touch{};
        std::array<TouchPoint, maximumPoints> points{};
    };

    // Common borrowed controller API. Board owns initialization and teardown.
    class TouchController
    {
        static_assert(detail::NativeTouchData::maximumPoints <= TouchData::maximumPoints);

    public:
        static constexpr auto defaultI2cAddress = detail::NativeTouch::defaultI2cAddress;
        static constexpr auto defaultI2cFrequencyHz = detail::NativeTouch::defaultI2cFrequencyHz;

        explicit TouchController(pdl::I2cDevice &device) noexcept : native_(device) {}
        TouchController(const TouchController &) = delete;
        TouchController &operator=(const TouchController &) = delete;

        [[nodiscard]] static constexpr bool isValidI2cAddress(std::uint16_t address) noexcept
        {
#if T_PANEL_P4_BSP_IS_ROUND
            return address == defaultI2cAddress ||
                   address == pdl::touch::Gt911Touch::alternateI2cAddress;
#else
            return address == defaultI2cAddress;
#endif
        }

        [[nodiscard]] bool isInitialized() const noexcept { return native_.isInitialized(); }

        // busy means no new report: keep the previous input state. Only an ok
        // report with zero fingers means release. Errors leave data unchanged.
        [[nodiscard]] pdl::Status readTouchData(TouchData &data,
                                              std::uint32_t timeout_ms = 100) const
        {
            detail::NativeTouchData report{};
            const auto status = native_.readTouchData(report, timeout_ms);
            if (pdl::isError(status))
                return status;
            if (report.finger_count > report.points.size())
                return pdl::Status::invalid_data;
            TouchData result{};
            result.finger_count = report.finger_count;
#if T_PANEL_P4_BSP_IS_STANDARD
            result.edge_touch = report.edge_touch;
#endif
            for (std::size_t index = 0; index < report.finger_count; ++index)
            {
                const auto &point = report.points[index];
                result.points[index] = {point.x, point.y, point.strength,
#if T_PANEL_P4_BSP_IS_ROUND
                                        point.track_id
#else
                                        static_cast<std::uint8_t>(index)
#endif
                };
            }
            data = result;
            return pdl::Status::ok;
        }

        // Escape hatch for chip-specific diagnostics and power modes only.
        [[nodiscard]] detail::NativeTouch &native() noexcept { return native_; }

    private:
        friend class Board;
        [[nodiscard]] pdl::Status initialize(bool software_reset)
        {
#if T_PANEL_P4_BSP_IS_ROUND
            if (software_reset)
                return pdl::Status::unsupported;
            return native_.initialize();
#else
            return native_.initialize(software_reset);
#endif
        }
        [[nodiscard]] pdl::Status deinitialize() { return native_.deinitialize(); }
        detail::NativeTouch native_;
    };
}

namespace bsp
{
    using TouchController = T_PANEL_P4_BSP_NAMESPACE::TouchController;
    using TouchPoint = T_PANEL_P4_BSP_NAMESPACE::TouchPoint;
    using TouchData = T_PANEL_P4_BSP_NAMESPACE::TouchData;
}
