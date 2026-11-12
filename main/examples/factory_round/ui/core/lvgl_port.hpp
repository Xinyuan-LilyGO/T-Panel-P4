#pragma once

#include <esp_err.h>
#include <cstdint>

namespace bsp::t_panel_p4_round
{
class Board;
}

namespace factory::ui
{
[[nodiscard]] esp_err_t initializeLvgl(
    bsp::t_panel_p4_round::Board &board);
// Called from the shutdown worker before releasing touch/display resources.
void prepareLvglPowerDown(bool stop_display);
void resumeLvglDisplay();
// Runs on the LVGL thread before submitting its completed, inactive buffer.
using FramebufferOverlay = void (*)(std::uint8_t *framebuffer);
void setFramebufferOverlay(FramebufferOverlay overlay);
}
