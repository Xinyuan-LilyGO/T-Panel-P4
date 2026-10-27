#pragma once

#include <cstdint>

#include <esp_err.h>
#include <lvgl.h>

namespace factory::ui
{
struct LvglDisplayPort
{
    std::int32_t horizontal_resolution{};
    std::int32_t vertical_resolution{};
    lv_color_format_t color_format{LV_COLOR_FORMAT_UNKNOWN};
    void *primary_buffer{};
    void *secondary_buffer{};
    std::uint32_t buffer_size_bytes{};
    lv_display_render_mode_t render_mode{LV_DISPLAY_RENDER_MODE_PARTIAL};
    lv_display_flush_cb_t flush_callback{};
    lv_display_flush_wait_cb_t flush_wait_callback{};
    void *user_data{};
};

struct LvglInputPort
{
    lv_indev_read_cb_t read_callback{};
    void *user_data{};
};

struct LvglPort
{
    LvglDisplayPort display;
    LvglInputPort pointer_input;
    bool create_handler_task{true};
};

[[nodiscard]] esp_err_t initializeLvgl(const LvglPort &port);
}
