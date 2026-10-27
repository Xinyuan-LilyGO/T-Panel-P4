#pragma once

#include <stdint.h>

#include <ui/core/ui.h>

static inline lv_obj_t *page_ui_label(
    lv_obj_t *parent,
    const char *text,
    const lv_font_t *font,
    uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static inline lv_obj_t *page_ui_panel(
    lv_obj_t *parent,
    lv_coord_t x,
    lv_coord_t y,
    lv_coord_t width,
    lv_coord_t height,
    uint32_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 14, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

static inline void page_ui_apply_panel_style(lv_obj_t *object, uint32_t color)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static inline lv_obj_t *page_ui_corner_line(
    lv_obj_t *parent,
    lv_coord_t x,
    lv_coord_t y,
    lv_coord_t width,
    lv_coord_t height,
    uint32_t color)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, width, height);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_shadow_width(line, 6, 0);
    lv_obj_set_style_shadow_spread(line, 1, 0);
    lv_obj_set_style_shadow_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_opa(line, LV_OPA_40, 0);
    lv_obj_remove_flag(line, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE |
                                                         LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(line, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return line;
}

static inline void page_ui_add_corner_brackets(
    lv_obj_t *parent,
    lv_coord_t x,
    lv_coord_t y,
    lv_coord_t width,
    lv_coord_t height,
    uint32_t color,
    lv_coord_t outward)
{
    const lv_coord_t arm = 28;
    const lv_coord_t stroke = 3;
    const lv_coord_t left = x - outward;
    const lv_coord_t top = y - outward;
    const lv_coord_t right = x + width + outward - stroke;
    const lv_coord_t bottom = y + height + outward - stroke;

    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    page_ui_corner_line(parent, left, top, arm, stroke, color);
    page_ui_corner_line(parent, left, top, stroke, arm, color);
    page_ui_corner_line(parent, right - arm + stroke, top, arm, stroke, color);
    page_ui_corner_line(parent, right, top, stroke, arm, color);
    page_ui_corner_line(parent, left, bottom, arm, stroke, color);
    page_ui_corner_line(parent, left, bottom - arm + stroke, stroke, arm, color);
    page_ui_corner_line(parent, right - arm + stroke, bottom, arm, stroke, color);
    page_ui_corner_line(parent, right, bottom - arm + stroke, stroke, arm, color);
}

/* Shared two-corner treatment for selectable option containers. */
static inline void page_ui_add_top_left_bottom_right_brackets(
    lv_obj_t *parent,
    lv_coord_t width,
    lv_coord_t height,
    uint32_t color,
    lv_coord_t outward = 0)
{
    constexpr lv_coord_t arm = 20;
    constexpr lv_coord_t stroke = 2;
    const lv_coord_t left = -outward;
    const lv_coord_t top = -outward;
    (void)width;
    (void)height;

    lv_obj_add_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    page_ui_corner_line(parent, left, top, arm, stroke, color);
    page_ui_corner_line(parent, left, top, stroke, arm, color);
    lv_obj_t *bottom_right_horizontal =
        page_ui_corner_line(parent, 0, 0, arm, stroke, color);
    lv_obj_align(bottom_right_horizontal, LV_ALIGN_BOTTOM_RIGHT, outward, outward);
    lv_obj_t *bottom_right_vertical =
        page_ui_corner_line(parent, 0, 0, stroke, arm, color);
    lv_obj_align(bottom_right_vertical, LV_ALIGN_BOTTOM_RIGHT, outward, outward);
}

static inline lv_obj_t *page_ui_button(
    lv_obj_t *parent,
    const char *symbol,
    const char *text,
    lv_coord_t width,
    lv_coord_t height,
    lv_event_cb_t callback,
    void *user_data)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PRIMARY), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    if (callback != NULL)
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *content = lv_label_create(button);
    if (text != NULL && symbol != NULL && symbol[0] != '\0')
        lv_label_set_text_fmt(
            content, "%s  %s", symbol, text);
    else if (text != NULL)
        lv_label_set_text(content, text);
    else
        lv_label_set_text(content, symbol != NULL ? symbol : "");
    lv_obj_set_style_text_font(content, &lv_font_ShareTechMono1_20, 0);
    lv_obj_set_style_text_color(content, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(content);
    return button;
}

static inline lv_obj_t *page_ui_icon_button(
    lv_obj_t *parent,
    const char *symbol,
    lv_coord_t width,
    lv_coord_t height,
    lv_event_cb_t callback,
    void *user_data)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL_HL), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PRIMARY), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    if (callback != NULL)
        lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_ShareTechMono1_24, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(label);
    return button;
}

static inline void page_ui_setup(
    Page *page,
    const char *title,
    const char *subtitle)
{
    lv_obj_set_style_bg_color(page->root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *heading = page_ui_label(
        page->root, title, &lv_font_ShareTechMono1_24, UI_TEXT);
    lv_obj_set_pos(heading, 28, 54);
    lv_obj_t *caption = page_ui_label(
        page->root, subtitle, &lv_font_ShareTechMono1_20, UI_MUTED);
    lv_obj_align(caption, LV_ALIGN_TOP_RIGHT, -28, 58);
}

static inline void page_ui_back_gesture(
    Page *page,
    GestureDirection direction)
{
    (void)page;
    if (direction == GESTURE_RIGHT)
        ui_page_switch_async(PAGE_HOME);
}
