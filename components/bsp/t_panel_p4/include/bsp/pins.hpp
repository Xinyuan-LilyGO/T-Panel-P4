#pragma once

#include <bsp/hardware.hpp>

#if T_PANEL_P4_BSP_IS_STANDARD
#include <bsp/pins/standard.hpp>
#elif T_PANEL_P4_BSP_IS_ROUND
#include <bsp/pins/round.hpp>
#elif T_PANEL_P4_BSP_IS_RECT
#include <bsp/pins/rect.hpp>
#endif

namespace bsp
{
    namespace pins = T_PANEL_P4_BSP_NAMESPACE::pins;
}
