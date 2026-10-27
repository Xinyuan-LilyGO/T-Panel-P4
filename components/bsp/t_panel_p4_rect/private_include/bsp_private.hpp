#pragma once

#include <esp_err.h>

#include <pdl/core/status.hpp>

namespace bsp::t_panel_p4_rect::detail
{
[[nodiscard]] esp_err_t toEspError(pdl::Status status) noexcept;
}
