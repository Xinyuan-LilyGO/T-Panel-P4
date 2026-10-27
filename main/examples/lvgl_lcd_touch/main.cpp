#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <esp_attr.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <bsp/t_panel_p4_standard.hpp>
#include <pdl/core/status.hpp>
#include <pdl/drivers/touch/jadard/jadard.hpp>

namespace
{
static_assert(
    LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR == 5 &&
        LVGL_VERSION_PATCH == 0,
    "This example requires LVGL 9.5.0");

constexpr char kTag[] = "lvgl_lcd_touch";
constexpr int kDisplayWidth =
    bsp::t_panel_p4_standard::Board::displayWidth;
constexpr int kDisplayHeight =
    bsp::t_panel_p4_standard::Board::displayHeight;
constexpr std::size_t kDrawBufferLines = 48;
constexpr std::uint32_t kTouchTimeoutMs = 20;
constexpr std::uint32_t kMinimumLoopDelayMs = 5;
constexpr std::uint32_t kMaximumLoopDelayMs = 20;

struct Application
{
    bsp::t_panel_p4_standard::Board board;
    pdl::touch::JadardTouch *touch{};
    lv_obj_t *coordinate_label{};
    std::uint16_t last_x{};
    std::uint16_t last_y{};
    bool has_coordinate{};
};

Application application;

std::uint32_t lvglTickMilliseconds()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

void flushDisplay(
    lv_display_t *display,
    const lv_area_t *area,
    std::uint8_t *pixel_map)
{
    auto panel = static_cast<esp_lcd_panel_handle_t>(
        lv_display_get_user_data(display));
    const esp_err_t error = esp_lcd_panel_draw_bitmap(
        panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        pixel_map);
    if (error != ESP_OK)
    {
        ESP_LOGE(kTag, "Display flush failed: %s", esp_err_to_name(error));
        lv_display_flush_ready(display);
    }
}

bool IRAM_ATTR notifyFlushReady(
    esp_lcd_panel_handle_t,
    esp_lcd_dpi_panel_event_data_t *,
    void *user_context)
{
    auto *display = static_cast<lv_display_t *>(user_context);
    lv_display_flush_ready(display);
    return false;
}

void readTouch(lv_indev_t *input_device, lv_indev_data_t *input_data)
{
    auto *app = static_cast<Application *>(
        lv_indev_get_user_data(input_device));
    if (app == nullptr || app->touch == nullptr)
    {
        input_data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    pdl::touch::JadardTouchData report{};
    const pdl::Status status =
        app->touch->readTouchData(report, kTouchTimeoutMs);
    if (pdl::isError(status) || report.finger_count == 0U)
    {
        input_data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    const auto &point = report.points[0];
    const std::uint16_t x = std::min<std::uint16_t>(
        point.x,
        static_cast<std::uint16_t>(kDisplayWidth - 1));
    const std::uint16_t y = std::min<std::uint16_t>(
        point.y,
        static_cast<std::uint16_t>(kDisplayHeight - 1));

    input_data->point.x = x;
    input_data->point.y = y;
    input_data->state = LV_INDEV_STATE_PRESSED;

    if (app->coordinate_label != nullptr &&
        (!app->has_coordinate || app->last_x != x || app->last_y != y))
    {
        app->last_x = x;
        app->last_y = y;
        app->has_coordinate = true;
        lv_label_set_text_fmt(
            app->coordinate_label,
            "X: %u  Y: %u",
            static_cast<unsigned int>(x),
            static_cast<unsigned int>(y));
    }
}

esp_err_t initializeLvgl(Application &app)
{
    lv_init();
    lv_tick_set_cb(lvglTickMilliseconds);

    lv_display_t *display = lv_display_create(kDisplayWidth, kDisplayHeight);
    ESP_RETURN_ON_FALSE(
        display != nullptr,
        ESP_ERR_NO_MEM,
        kTag,
        "Create LVGL display failed");

    lv_display_set_user_data(display, app.board.displayPanel());
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);

    constexpr std::size_t draw_buffer_size =
        kDisplayWidth * kDrawBufferLines * sizeof(std::uint16_t);
    void *first_buffer = heap_caps_aligned_calloc(
        LV_DRAW_BUF_ALIGN,
        1,
        draw_buffer_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *second_buffer = heap_caps_aligned_calloc(
        LV_DRAW_BUF_ALIGN,
        1,
        draw_buffer_size,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(
        first_buffer != nullptr && second_buffer != nullptr,
        ESP_ERR_NO_MEM,
        kTag,
        "Allocate LVGL draw buffers failed");

    lv_display_set_buffers(
        display,
        first_buffer,
        second_buffer,
        draw_buffer_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flushDisplay);

    esp_lcd_dpi_panel_event_callbacks_t panel_callbacks{};
    panel_callbacks.on_color_trans_done = notifyFlushReady;
    ESP_RETURN_ON_ERROR(
        esp_lcd_dpi_panel_register_event_callbacks(
            app.board.displayPanel(),
            &panel_callbacks,
            display),
        kTag,
        "Register display flush callback failed");

    lv_indev_t *input_device = lv_indev_create();
    ESP_RETURN_ON_FALSE(
        input_device != nullptr,
        ESP_ERR_NO_MEM,
        kTag,
        "Create LVGL touch input failed");
    lv_indev_set_type(input_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input_device, display);
    lv_indev_set_user_data(input_device, &app);
    lv_indev_set_read_cb(input_device, readTouch);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF1F3F5), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    app.coordinate_label = lv_label_create(screen);
    ESP_RETURN_ON_FALSE(
        app.coordinate_label != nullptr,
        ESP_ERR_NO_MEM,
        kTag,
        "Create coordinate label failed");
    lv_label_set_text(app.coordinate_label, "X: ---  Y: ---");
    lv_obj_set_style_text_color(
        app.coordinate_label,
        lv_color_hex(0x111827),
        LV_PART_MAIN);
    lv_obj_set_style_text_font(
        app.coordinate_label,
        &lv_font_montserrat_24,
        LV_PART_MAIN);
    lv_obj_align(app.coordinate_label, LV_ALIGN_TOP_LEFT, 16, 16);

    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(application.board.initialize());
    ESP_ERROR_CHECK(application.board.initializeDisplay());
    ESP_ERROR_CHECK(application.board.initializeTouch());

    application.touch = application.board.touchController();
    ESP_ERROR_CHECK(
        application.touch != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);
    ESP_ERROR_CHECK(initializeLvgl(application));
    ESP_ERROR_CHECK(application.board.setBacklightBrightness(100));

    ESP_LOGI(kTag, "LVGL 9.5 display and touch input initialized");
    while (true)
    {
        std::uint32_t delay_ms = lv_timer_handler();
        delay_ms = std::max(delay_ms, kMinimumLoopDelayMs);
        delay_ms = std::min(delay_ms, kMaximumLoopDelayMs);
        TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
        if (delay_ticks == 0)
            delay_ticks = 1;
        vTaskDelay(delay_ticks);
    }
}
