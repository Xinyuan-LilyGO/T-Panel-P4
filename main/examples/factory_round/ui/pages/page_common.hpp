#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <lvgl.h>
#include <lvgl_page_manager.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define UI_BLUE_BG 0x1d25b6
#define UI_MAIN 0xff6500
#define UI_SECOND 0xe235ac
#define UI_BLACK 0x0D0D0D
#define UI_WHITE 0xF5F5F5
#define UI_WIHTE UI_WHITE
#define UI_GTAY 0xa1a1a1

#ifdef __cplusplus
}
#endif

enum class PageId : int8_t
{
    PAGE_START = 0,
    PAGE_HOME,
    PAGE_MUSIC,
    PAGE_RECORD,
    PAGE_CAMERA,
    PAGE_LORA,
    PAGE_FILE,
    PAGE_BMU,
    PAGE_SET,
    PAGE_SIZE,
};

/** Shared page UI helper: create the full-screen page background. */
static inline lv_obj_t *ui_create_background(lv_obj_t *parent)
{
    if (parent == nullptr)
        return nullptr;

    lv_obj_t *background = lv_obj_create(parent);
    if (background == nullptr)
        return nullptr;

    lv_obj_set_pos(background, 0, 0);
    lv_obj_set_size(background, 720, 720);
    lv_obj_set_style_radius(background, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(background, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(background, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(background, LV_OPA_COVER, LV_PART_MAIN);

    // The descriptor and its stop arrays must outlive the object because LVGL
    // keeps a pointer to the gradient descriptor in the style.
    static lv_grad_dsc_t radial_gradient{};
    static const lv_color_t colors[] = {
        lv_color_hex(UI_BLUE_BG),
        lv_color_hex(UI_BLACK),
    };
    static const lv_opa_t opas[] = {
        LV_OPA_COVER,
        LV_OPA_COVER,
    };
    static const uint8_t fractions[] = {0, 255};

    lv_grad_init_stops(
        &radial_gradient,
        colors,
        opas,
        fractions,
        2);
    lv_grad_radial_init(
        &radial_gradient,
        360,
        360,
        720,
        360,
        LV_GRAD_EXTEND_PAD);
    lv_obj_set_style_bg_grad(
        background,
        &radial_gradient,
        LV_PART_MAIN);

    lv_obj_remove_flag(
        background,
        static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    return background;
}

static inline lv_obj_t *page_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    if (parent == nullptr || text == nullptr || font == nullptr)
        return nullptr;

    lv_obj_t *label = lv_label_create(parent);
    if (label == nullptr)
        return nullptr;

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/** Apply the shared translucent circular icon style with a soft white halo. */
static inline void page_ui_apply_icon_style(lv_obj_t *icon)
{
    if (icon == nullptr)
        return;

    // LVGL retains this descriptor, so it must outlive every styled icon.
    static lv_grad_dsc_t gradient{};
    static bool initialized = false;
    if (!initialized)
    {
        static const lv_color_t colors[] = {
            lv_color_hex(UI_WIHTE), lv_color_hex(UI_BLACK)};
        static const lv_opa_t opas[] = {LV_OPA_COVER, LV_OPA_COVER};
        static const uint8_t fractions[] = {0, 255};
        lv_grad_init_stops(&gradient, colors, opas, fractions, 2);
        lv_grad_linear_init(&gradient, 0, 0, 0, 80, LV_GRAD_EXTEND_PAD);
        initialized = true;
    }

    lv_obj_set_style_bg_opa(icon, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_bg_grad(icon, &gradient, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(icon, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
    lv_obj_set_style_border_opa(icon, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(icon, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(icon, LV_OPA_10, LV_PART_MAIN);
    // A wider blur produces a soft halo instead of a hard white ring.
    lv_obj_set_style_shadow_spread(icon, 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(icon, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_x(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(icon, 0, LV_PART_MAIN);
}

static inline lv_obj_t *page_ui_container(lv_obj_t *parent, uint32_t width, uint32_t height)
{
    if (parent == nullptr)
        return nullptr;

    lv_obj_t *container = lv_obj_create(parent);
    if (container == nullptr)
        return nullptr;

    lv_obj_set_size(container, width, height);
    lv_obj_set_style_radius(container, 28, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);

    return container;
}

/** Return to the Home page when the user swipes right on a child page. */
static inline void page_ui_back_gesture(Page *, GestureDirection direction)
{
    if (direction == GESTURE_RIGHT)
        (void)ui_page_switch_async(PAGE_HOME);
}
