#include "page_common.h"

#include <ui/core/status_bar.h>
#include <string.h>

extern "C" {
extern const lv_image_dsc_t _jijiawaiqiebiankuang_RGB565A8_720x720;
extern const lv_image_dsc_t _bot_left_checker_RGB565A8_110x32;
extern const lv_image_dsc_t _bot_right_stripes_RGB565A8_120x32;
}

typedef struct
{
    const char *name;
    const char *icon;
    PageType page;
} home_app_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *frame;
    lv_opa_t frame_opa;
    lv_obj_t *status_bar;
    lv_obj_t *logo;
    lv_timer_t *logo_gradient_timer;
    uint16_t logo_gradient_phase;
} home_page_ui_t;

#define HOME_STATUS_ANIM_DURATION_MS 700
#define HOME_STATUS_START_Y (-48)
#define HOME_FRAME_ANIM_DURATION_MS 240
#define HOME_LOGO_ANIM_DURATION_MS 850
#define HOME_LOGO_START_Y 96
#define HOME_APP_CARD_WIDTH 184
#define HOME_APP_CARD_HEIGHT 250
#define HOME_APP_GRID_X 68
#define HOME_APP_GRID_Y 103
#define HOME_APP_GRID_GAP_X 12
#define HOME_APP_GRID_GAP_Y 20

static home_page_ui_t s_home_ui;
static bool s_home_status_animation_started;

static void home_logo_gradient_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_home_ui.logo == NULL)
        return;

    constexpr uint32_t colors[] = {CYBER_YELLOW_GREEN, CYBER_CYAN, CYBER_MAGENTA};
    constexpr uint16_t segment_size = 256U;
    constexpr uint16_t cycle_size = segment_size * 3U;
    const uint16_t phase = s_home_ui.logo_gradient_phase;
    const uint8_t segment = static_cast<uint8_t>(phase / segment_size);
    const uint8_t fraction = static_cast<uint8_t>(phase % segment_size);
    const uint8_t next_next = static_cast<uint8_t>((segment + 2U) % 3U);

    const lv_color_t text = lv_color_mix(
        lv_color_hex(colors[segment]), lv_color_hex(colors[next_next]), fraction);
    /* Keep the animated color treatment on the lettering only. */
    lv_obj_set_style_text_color(s_home_ui.logo, text, 0);
    s_home_ui.logo_gradient_phase =
        static_cast<uint16_t>((phase + 4U) % cycle_size);
}

static const home_app_t s_home_apps[] = {
    {"Music", LV_SYMBOL_AUDIO, PAGE_MUSIC},
    {"Record", LV_SYMBOL_DRIVE, PAGE_RECORD},
    {"Camera", LV_SYMBOL_IMAGE, PAGE_CAMERA},
    {"Lora", LV_SYMBOL_WIFI, PAGE_LORA},
    {"File", LV_SYMBOL_FILE, PAGE_FILE},
    {"Set", LV_SYMBOL_SETTINGS, PAGE_SET},
};

static void home_app_clicked_cb(lv_event_t *event)
{
    const home_app_t *app = static_cast<const home_app_t *>(lv_event_get_user_data(event));
    if (app != NULL)
    {
        ui_page_switch_async(app->page);
    }
}

static void home_translate_y_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), value, 0);
}

static void home_app_create(lv_obj_t *parent, const home_app_t *app, int index)
{
    lv_obj_t *tile = lv_obj_create(parent);
    const int column = index % 3;
    const int row = index / 3;
    lv_obj_set_size(tile, HOME_APP_CARD_WIDTH, HOME_APP_CARD_HEIGHT);
    lv_obj_set_pos(tile,
                   HOME_APP_GRID_X + column * (HOME_APP_CARD_WIDTH + HOME_APP_GRID_GAP_X),
                   HOME_APP_GRID_Y + row * (HOME_APP_CARD_HEIGHT + HOME_APP_GRID_GAP_Y));
    lv_obj_set_style_bg_color(tile, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_bg_opa(tile, 20, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_40, 0);
    lv_obj_set_style_radius(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_border_width(tile, 0, LV_STATE_PRESSED);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(tile, home_app_clicked_cb, LV_EVENT_CLICKED, (void *)app);
    lv_obj_t *number = lv_label_create(tile);
    lv_obj_set_pos(number, 8, 8);
    lv_label_set_text_fmt(number, "#%02d", index + 1);
    lv_obj_set_style_text_color(number, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_text_opa(number, LV_OPA_50, 0);
    lv_obj_set_style_text_font(number, &lv_font_ShareTechMono1_16, 0);

    lv_obj_t *icon = lv_label_create(tile);
    lv_label_set_text(icon, app->icon);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(icon, &lv_font_ShareTechMono1_50, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_text_opa(icon, LV_OPA_70, 0);
    lv_obj_set_style_text_opa(icon, LV_OPA_100, LV_STATE_PRESSED);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_width(name, HOME_APP_CARD_WIDTH - 16);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_label_set_text(name, app->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(name, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_text_font(name, &lv_font_ShareTechMono1_20, 0);
    lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
    lv_obj_set_style_text_opa(name, LV_OPA_100, LV_STATE_PRESSED);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);

}

static void home_apps_create(lv_obj_t *parent)
{
    for (size_t i = 0; i < sizeof(s_home_apps) / sizeof(s_home_apps[0]); ++i)
    {
        home_app_create(parent, &s_home_apps[i], (int)i);
    }
}

static void home_status_translate_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(var), value, 0);
}

static void home_frame_opa_cb(void *var, int32_t value)
{
    /* A full-screen RGB565A8 image is expensive to blend. Quantize the fade
     * to five visible steps and invalidate only when a step changes. */
    const lv_opa_t target = value >= LV_OPA_COVER
                                ? static_cast<lv_opa_t>(LV_OPA_COVER)
                                : static_cast<lv_opa_t>((value / 64) * 64);
    if (s_home_ui.frame_opa == target)
        return;
    s_home_ui.frame_opa = target;
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), target, 0);
}

static void home_page_create(Page *page)
{
    s_home_ui.root = page->root;
    lv_obj_set_style_bg_color(s_home_ui.root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_pad_all(s_home_ui.root, 0, 0);
    lv_obj_set_scrollbar_mode(s_home_ui.root, LV_SCROLLBAR_MODE_OFF);

    s_home_ui.frame = lv_image_create(s_home_ui.root);
    lv_image_set_src(s_home_ui.frame, &_jijiawaiqiebiankuang_RGB565A8_720x720);
    lv_obj_set_pos(s_home_ui.frame, 0, 0);
    lv_obj_set_size(s_home_ui.frame, 720, 720);
    s_home_ui.frame_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_home_ui.frame, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_home_ui.frame, LV_OBJ_FLAG_CLICKABLE);

    status_bar_create();
    s_home_ui.status_bar = status_bar_get_container();
    lv_obj_set_style_translate_y(s_home_ui.status_bar, HOME_STATUS_START_Y, 0);

    home_apps_create(s_home_ui.root);

    lv_obj_t *bottom_left = lv_image_create(s_home_ui.root);
    lv_image_set_src(bottom_left, &_bot_left_checker_RGB565A8_110x32);
    lv_obj_set_pos(bottom_left, 20, 668);
    lv_obj_remove_flag(bottom_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *bottom_right = lv_image_create(s_home_ui.root);
    lv_image_set_src(bottom_right, &_bot_right_stripes_RGB565A8_120x32);
    lv_obj_set_pos(bottom_right, 584, 668);
    lv_obj_remove_flag(bottom_right, LV_OBJ_FLAG_CLICKABLE);

    s_home_ui.logo = page_ui_label(s_home_ui.root, "LILYGO", &lv_font_ShareTechMono1_32, CYBER_YELLOW);
    lv_obj_set_width(s_home_ui.logo, 400);
    lv_obj_set_height(s_home_ui.logo, 40);
    lv_obj_set_style_bg_opa(s_home_ui.logo, LV_OPA_TRANSP, 0);
    lv_obj_align(s_home_ui.logo, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_align(s_home_ui.logo, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_home_ui.logo, LV_OPA_70, 0);
    s_home_ui.logo_gradient_phase = 0U;
    s_home_ui.logo_gradient_timer = lv_timer_create(
        home_logo_gradient_timer_cb, 40U, NULL);
}

static void home_page_enter(Page *page)
{
    (void)page;
    if (s_home_ui.status_bar == NULL || s_home_status_animation_started)
    {
        return;
    }
    s_home_status_animation_started = true;

    s_home_ui.frame_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_home_ui.frame, LV_OPA_TRANSP, 0);
    lv_anim_t frame_anim;
    lv_anim_init(&frame_anim);
    lv_anim_set_var(&frame_anim, s_home_ui.frame);
    lv_anim_set_values(&frame_anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&frame_anim, HOME_FRAME_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&frame_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&frame_anim, home_frame_opa_cb);
    lv_anim_start(&frame_anim);

    lv_obj_set_style_translate_y(s_home_ui.logo, HOME_LOGO_START_Y, 0);
    lv_anim_t logo_anim;
    lv_anim_init(&logo_anim);
    lv_anim_set_var(&logo_anim, s_home_ui.logo);
    lv_anim_set_values(&logo_anim, HOME_LOGO_START_Y, 0);
    lv_anim_set_duration(&logo_anim, HOME_LOGO_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&logo_anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&logo_anim, home_translate_y_cb);
    lv_anim_start(&logo_anim);

    lv_obj_set_style_translate_y(s_home_ui.status_bar, HOME_STATUS_START_Y, 0);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_home_ui.status_bar);
    lv_anim_set_values(&anim, HOME_STATUS_START_Y, 0);
    lv_anim_set_duration(&anim, HOME_STATUS_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, home_status_translate_cb);
    lv_anim_start(&anim);
}

static void home_page_leave(Page *page)
{
    (void)page;
    (void)ui_page_destroy(PAGE_HOME);
}

static void home_page_destroy(Page *page)
{
    (void)page;
    if (s_home_ui.logo_gradient_timer != NULL)
        lv_timer_del(s_home_ui.logo_gradient_timer);
    s_home_status_animation_started = false;
    memset(&s_home_ui, 0, sizeof(s_home_ui));
}

extern "C" void home_page_register(void)
{
    Page page{};

    page.id = PAGE_HOME;

    page.name = "home";

    page.on_create = home_page_create;

    page.on_enter = home_page_enter;

    page.on_destroy = home_page_destroy;

    page.on_leave = home_page_leave;
    ui_page_register(&page);
}