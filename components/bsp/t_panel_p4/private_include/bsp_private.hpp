#pragma once

#include <bsp/hardware.hpp>

#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>

#include <pdl/core/status.hpp>
namespace bsp::T_PANEL_P4_BSP_NAMESPACE::detail
{
    struct DisplayHardwareConfig
    {
        const char *panel_name;
        float lane_bit_rate_mbps;
        float pixel_clock_mhz;
        esp_lcd_video_timing_t video_timing;
    };

    [[nodiscard]] const DisplayHardwareConfig &displayHardwareConfig() noexcept;
    [[nodiscard]] esp_err_t toEspError(pdl::Status status) noexcept;
}
