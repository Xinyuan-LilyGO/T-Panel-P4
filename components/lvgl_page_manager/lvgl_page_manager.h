#ifndef LVGL_PAGE_MANAGER_H
#define LVGL_PAGE_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "lvgl.h"

/*
 * Platform selection:
 * - RT-Thread: define LV_PM_USE_RTTHREAD or include RT-Thread build macros
 * - Arduino-ESP32: ARDUINO
 * - ESP-IDF: ESP_PLATFORM
 *
 * Optional config:
 * - LV_PM_SKIP_DISPLAY_INIT: do not call littlevgl2rtt_init() on RT-Thread
 * - LV_PM_CALL_LV_INIT: call lv_init() in ui_init() on ESP-IDF/Arduino
 * - LV_PM_TASK_STACK_SIZE: UI task stack size, default 8 KB
 * - LV_PM_TASK_PRIORITY: UI task priority, default 5 on FreeRTOS
 */
#if defined(LV_PM_USE_RTTHREAD) || defined(RTTHREAD_VERSION) || defined(__RTTHREAD__)
#define LV_PM_PLATFORM_RTTHREAD 1
#include "rtthread.h"
#elif defined(ARDUINO)
#define LV_PM_PLATFORM_ARDUINO 1
#elif defined(ESP_PLATFORM)
#define LV_PM_PLATFORM_ESP_IDF 1
#else
#define LV_PM_PLATFORM_NONE 1
#endif

#if defined(LV_PM_PLATFORM_RTTHREAD)
#ifndef RT_EOK
#define RT_EOK 0
#endif
#ifndef RT_ERROR
#define RT_ERROR 1
#endif
#ifndef RT_EINVAL
#define RT_EINVAL 22
#endif
#ifndef RT_EBUSY
#define RT_EBUSY 16
#endif
#ifndef RT_NULL
#define RT_NULL NULL
#endif
#else
typedef int rt_err_t;
#ifndef RT_EOK
#define RT_EOK 0
#endif
#ifndef RT_ERROR
#define RT_ERROR 1
#endif
#ifndef RT_EINVAL
#define RT_EINVAL 22
#endif
#ifndef RT_EBUSY
#define RT_EBUSY 16
#endif
#ifndef RT_NULL
#define RT_NULL NULL
#endif
#endif

#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR < 9)
#define lv_image_dsc_t lv_img_dsc_t
#endif

#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 720
#endif

#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 720
#endif

typedef enum {
    PAGE_START = 0,
    PAGE_HOME,
    PAGE_MUSIC,
    PAGE_RECORD,
    PAGE_CAMERA,
    PAGE_LORA,
    PAGE_FILE,
    PAGE_BMU,
    PAGE_SET,
    PAGE_SENSOR,
    PAGE_SELF_TEST,
    PAGE_SELF_TEST_RUN,
    PAGE_SIZE,
} PageType;

typedef enum {
    APP_1 = 0,
    APP_SIZE,
} AppType;

typedef enum {
    GESTURE_NONE  = 0,
    GESTURE_LEFT  = (1 << 0),
    GESTURE_RIGHT = (1 << 1),
    GESTURE_UP    = (1 << 2),
    GESTURE_DOWN  = (1 << 3),
} GestureDirection;

typedef struct Page Page;

typedef void (*page_lifecycle_cb_t)(Page *page);
typedef void (*page_group_cb_t)(lv_group_t *group);
typedef void (*page_timer_cb_t)(Page *page);
typedef void (*page_gesture_cb_t)(Page *page, GestureDirection direction);

struct Page {
    /* Runtime objects. They are owned by page manager after registration. */
    lv_obj_t *root;
    lv_group_t *group;
    lv_timer_t *timer;

    /* Kept for old user code compatibility. New code should not depend on these. */
    lv_obj_t *last_root;
    lv_obj_t *last_group_obj;

    const char *name;
    PageType id;
    AppType app_id;
    bool is_created;

    page_group_cb_t group_cb;
    page_timer_cb_t page_timer;
    page_gesture_cb_t on_gesture;
    page_lifecycle_cb_t on_create;
    page_lifecycle_cb_t on_destroy;
    page_lifecycle_cb_t on_enter;
    page_lifecycle_cb_t on_leave;

    uint32_t timer_interval_ms;
    uint32_t timer_counter_ms;

    /* Legacy aliases. Prefer timer_interval_ms/timer_counter_ms in new code. */
    uint16_t timer_interval;
    uint32_t timer_counter;

    void *user_data;
};

typedef struct app_info {
    const lv_image_dsc_t *icon_dsc;
    const char *name;
    const char *text_title[8];
    const char *text_data[8];
    uint8_t text_count;
    uint8_t id;
} app_info_t;
typedef app_info_t app_info;

typedef enum {
    UI_EVENT_NONE = 0,
    UI_EVENT_SIZE,
} ui_event_type_t;

typedef enum {
    UI_EVENT_KEY = 0,
    UI_UPDATE_SIZE,
} ui_update_type_t;

typedef enum {
    INIT_ERROR_NONE = 0,
    INIT_SD_ERROR = (1 << 0),
    INIT_BMU_ERROR = (1 << 1),
    INIT_LORA_ERROR = (1 << 2),
    INIT_AUDIO_ERROR = (1 << 3),
    INIT_SLAVE_ERROR = (1 << 4),
    INIT_CAMERA_ERROR = (1 << 5),
    INIT_SENSOR_ERROR = (1 << 6),
    INIT_MOTOR_ERROR = (1 << 7),
    INIT_ERROR_SIZE,
} init_error_t;
extern init_error_t s_init_error;

typedef struct device_switch {
    bool wifi_enable;
} device_switch_t;

rt_err_t ui_init(void);

rt_err_t ui_page_register(const Page *page);
rt_err_t ui_page_switch(PageType id);
rt_err_t ui_page_switch_async(PageType id);
rt_err_t ui_page_destroy(PageType id);
Page *ui_page_get(PageType id);
Page *ui_page_get_current(void);

void ui_lock(void);
void ui_unlock(void);

device_switch_t *get_device_switch(void);

/* UI color */
#define LV_COLOR_THEME_BLACK         0x000000
#define LV_COLOR_THEME_WHITE         0xffffff
#define LV_COLOR_THEME_GREEN         0x00ff00
#define LV_COLOR_THEME_MATTER        0x32B67A
#define LV_COLOR_THEME_BUSINESS      0x167c80
#define LV_COLOR_THEME_LE_CARNAVAL   0x005397
#define LV_COLOR_THEME_YELLOW        0xff9d00
#define LV_COLOR_THEME_YELLOW_2        0xF0CF61
#define LV_COLOR_WARM_RED            0xff0027
#define LV_COLOR_BTN_BLUE            0x2195f6
#define LV_COLOR_BTN_BLUE_2            0x0BBCD6
#define LV_COLOR_THEME_SWAN_DIVE     0xF9F7E8
#define LV_COLOR_THEME_GRAY          0xa1a1a1
#define LV_COLOR_THEME_DIM_GRAY      0x3a3a3a

#define COLOR_BG        0x101216
#define COLOR_PANEL     0x1A1D24
#define COLOR_PANEL_2   0x222733
#define COLOR_PANEL_3   0x2a2a2a
#define COLOR_TEXT      0xF5F7FA
#define COLOR_MUTED     0x9AA3B2
#define COLOR_ACCENT    0x4CC9F0
#define COLOR_SUCCESS   0x7EE787

#define UI_BG          0x050510
#define UI_PANEL       0x10101F
#define UI_PANEL_HL    0x18182A
#define UI_LINE        0x22304A
#define UI_TEXT        0xEAF7FF
#define UI_MUTED       0x7F8EA3
#define UI_PRIMARY     0x4CC9F0
#define UI_SECONDARY   0x58D68D
#define UI_WARN        0xFFE66D
#define UI_OK          0x00ff00
#define UI_ERROR       0xff0027

#define UI_COLOR_1     0x192234
#define UI_COLOR_2     0x223047
#define UI_COLOR_3     0x33445F
#define UI_COLOR_4     0xF5F7FC
#define UI_COLOR_5     0xACB7C9
#define UI_COLOR_6     0x087E73
#define UI_COLOR_7     0x153E3A
#define UI_COLOR_8     0x58D68D
#define UI_COLOR_9     0xFFC857
#define UI_COLOR_10     0xFF4D6D

#ifdef __cplusplus
}
#endif

#endif /* LVGL_PAGE_MANAGER_H */
