#pragma once

#include <esp_err.h>

namespace bsp::t_panel_p4_standard
{
class Board;
}

namespace factory::ui
{
[[nodiscard]] esp_err_t initializeStandardLvgl(
    bsp::t_panel_p4_standard::Board &board);
// Called from the shutdown worker before releasing touch/display resources.
void prepareStandardLvglPowerDown(bool stop_display);
void resumeStandardLvglDisplay();
}
