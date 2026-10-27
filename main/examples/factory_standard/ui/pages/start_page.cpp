#include "page_common.h"

#include <string.h>

#include <ui/core/status_bar.h>

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *logo_label;
    lv_obj_t *board_name_label;
} start_page_ui_t;

#define START_TEXT_ANIM_DELAY_MS       180
#define START_TEXT_ANIM_DURATION_MS    1100
#define START_LOGO_TRANSLATE_Y         32
#define START_BOARD_TRANSLATE_Y        (-22)
#define START_LINE_MIN_WIDTH           12
#define START_LINE_MAX_WIDTH           500
#define START_LINE_ANIM_DURATION_MS    1300
#define START_EXIT_ANIM_DURATION_MS    600
#define START_LINE_COLOR_LEFT          CYBER_CYAN
#define START_LINE_COLOR_RIGHT         CYBER_YELLOW_GREEN

static start_page_ui_t s_start_ui;
static lv_obj_t *s_start_line;
static lv_timer_t *s_start_switch_timer;

static void start_label_translate_y_cb(void *var, int32_t value);
static void start_label_opa_cb(void *var, int32_t value);
static void start_page_switch_timer_cb(lv_timer_t *timer);

static void start_line_width_cb(void *var, int32_t value)
{
    lv_obj_t *line = (lv_obj_t *)var;
    lv_obj_set_width(line, value);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 40);
}

static void start_page_exit_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    lv_obj_add_flag(s_start_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_start_ui.logo_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_start_ui.board_name_label, LV_OBJ_FLAG_HIDDEN);
    if (s_start_switch_timer == NULL)
    {
        s_start_switch_timer = lv_timer_create(
            start_page_switch_timer_cb, 500, NULL);
        lv_timer_set_repeat_count(s_start_switch_timer, 1);
    }
}

static void start_page_switch_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_start_switch_timer = NULL;
    ui_page_switch_async(PAGE_HOME);
}

static void start_obj_fade_out(lv_obj_t *obj, lv_anim_completed_cb_t completed_cb)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_values(&anim, lv_obj_get_style_opa(obj, static_cast<lv_part_t>(0)), LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, START_EXIT_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_exec_cb(&anim, start_label_opa_cb);
    if (completed_cb != NULL)
    {
        lv_anim_set_completed_cb(&anim, completed_cb);
    }
    lv_anim_start(&anim);
}

static void start_line_completed_cb(lv_anim_t *anim)
{
    (void)anim;

    Page *home_page = ui_page_get(PAGE_HOME);
    if (home_page == NULL || home_page->id != PAGE_HOME ||
        home_page->on_create == NULL)
    {
        return;
    }

    start_obj_fade_out(s_start_line, NULL);
    start_obj_fade_out(s_start_ui.board_name_label, NULL);
    start_obj_fade_out(s_start_ui.logo_label, start_page_exit_completed_cb);
}

static void start_line_enter_anim(lv_obj_t *line)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, line);
    lv_anim_set_values(&anim, START_LINE_MIN_WIDTH, START_LINE_MAX_WIDTH);
    lv_anim_set_duration(&anim, START_LINE_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&anim, start_line_width_cb);
    lv_anim_set_completed_cb(&anim, start_line_completed_cb);
    lv_anim_start(&anim);
}

static void start_label_translate_y_cb(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

static void start_label_opa_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void start_label_enter_anim(lv_obj_t *label, int32_t start_translate_y)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, label);
    lv_anim_set_values(&anim, start_translate_y, 0);
    lv_anim_set_delay(&anim, START_TEXT_ANIM_DELAY_MS);
    lv_anim_set_duration(&anim, START_TEXT_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, start_label_translate_y_cb);
    lv_anim_start(&anim);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, label);
    lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_delay(&anim, START_TEXT_ANIM_DELAY_MS);
    lv_anim_set_duration(&anim, START_TEXT_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, start_label_opa_cb);
    lv_anim_start(&anim);
}

static void start_page_create(Page *page)
{
    s_start_ui.root = page->root;

    s_start_ui.logo_label = lv_label_create(s_start_ui.root);
    lv_obj_align(s_start_ui.logo_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_start_ui.logo_label, "LILYGO");
    lv_obj_set_style_text_color(s_start_ui.logo_label, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_font(s_start_ui.logo_label, &lv_font_ShareTechMono1_50, 0);

    s_start_line = lv_obj_create(s_start_ui.root);
    lv_obj_set_size(s_start_line, START_LINE_MIN_WIDTH, 6);
    lv_obj_align(s_start_line, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_start_line,
                              lv_color_hex(START_LINE_COLOR_LEFT), 0);
    lv_obj_set_style_bg_grad_color(s_start_line,
                                   lv_color_hex(START_LINE_COLOR_RIGHT), 0);
    lv_obj_set_style_bg_grad_dir(s_start_line, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_main_stop(s_start_line, 0, 0);
    lv_obj_set_style_bg_grad_stop(s_start_line, 255, 0);
    lv_obj_set_style_bg_opa(s_start_line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_start_line, 3, 0);
    lv_obj_set_style_border_width(s_start_line, 0, 0);

    s_start_ui.board_name_label = lv_label_create(s_start_ui.root);
    lv_obj_align(s_start_ui.board_name_label, LV_ALIGN_CENTER, 0, 70);
    lv_label_set_text(s_start_ui.board_name_label, "T-Panel-P4");
    lv_obj_set_style_text_color(s_start_ui.board_name_label, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_text_font(s_start_ui.board_name_label, &lv_font_ShareTechMono1_24, 0);

    start_line_enter_anim(s_start_line);
    start_label_enter_anim(s_start_ui.logo_label, START_LOGO_TRANSLATE_Y);
    start_label_enter_anim(s_start_ui.board_name_label, START_BOARD_TRANSLATE_Y);
}

static void start_page_destroy(Page *page)
{
    (void)page;

    if (s_start_line != NULL)
    {
        lv_anim_delete(s_start_line, NULL);
    }
    if (s_start_switch_timer != NULL)
    {
        lv_timer_del(s_start_switch_timer);
        s_start_switch_timer = NULL;
    }
    if (s_start_ui.logo_label != NULL)
    {
        lv_anim_delete(s_start_ui.logo_label, NULL);
    }
    if (s_start_ui.board_name_label != NULL)
    {
        lv_anim_delete(s_start_ui.board_name_label, NULL);
    }

    s_start_line = NULL;
    memset(&s_start_ui, 0, sizeof(s_start_ui));
}

static void start_page_leave(Page *page)
{
    (void)page;
    (void)ui_page_destroy(PAGE_START);
}

extern "C" void start_page_register(void)
{
    Page page{};

    page.id = PAGE_START;

    page.name = "start";

    page.on_create = start_page_create;

    page.on_destroy = start_page_destroy;

    page.on_leave = start_page_leave;
    ui_page_register(&page);
}
