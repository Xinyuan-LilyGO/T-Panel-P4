#include "lvgl_page_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(LV_PM_PLATFORM_RTTHREAD)
extern rt_err_t littlevgl2rtt_init(const char *name);
#elif defined(LV_PM_PLATFORM_ESP_IDF) || defined(LV_PM_PLATFORM_ARDUINO)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if defined(LV_PM_PLATFORM_ESP_IDF)
#include "esp_log.h"
#endif
#if defined(LV_PM_PLATFORM_ARDUINO)
#include <Arduino.h>
#endif
#endif

#if defined(LV_PM_PLATFORM_RTTHREAD)
#define LV_PM_LOGE(...) log_e(__VA_ARGS__)
#define LV_PM_LOGD(...) log_d(__VA_ARGS__)
#elif defined(LV_PM_PLATFORM_ESP_IDF)
static const char *LV_PM_TAG = "lv_page_mgr";
#define LV_PM_LOGE(...) ESP_LOGE(LV_PM_TAG, __VA_ARGS__)
#define LV_PM_LOGD(...) ESP_LOGD(LV_PM_TAG, __VA_ARGS__)
#define LV_PM_LOGI(...) ESP_LOGI(LV_PM_TAG, __VA_ARGS__)
#elif defined(LV_PM_PLATFORM_ARDUINO)
#define LV_PM_LOGE(...)                    \
    do                                     \
    {                                      \
        Serial.printf("[E] " __VA_ARGS__); \
        Serial.println();                  \
    } while (0)
#define LV_PM_LOGD(...)                    \
    do                                     \
    {                                      \
        Serial.printf("[D] " __VA_ARGS__); \
        Serial.println();                  \
    } while (0)
#else
#define LV_PM_LOGE(...) ((void)0)
#define LV_PM_LOGD(...) ((void)0)
#endif

#ifndef LV_PM_TASK_STACK_SIZE
#define LV_PM_TASK_STACK_SIZE (10 * 1024)
#endif

#ifndef LV_PM_TASK_PRIORITY
#if defined(LV_PM_PLATFORM_RTTHREAD) && defined(RT_THREAD_PRIORITY_HIGH)
#define LV_PM_TASK_PRIORITY RT_THREAD_PRIORITY_HIGH
#else
#define LV_PM_TASK_PRIORITY 5
#endif
#endif

#ifndef LV_PM_TASK_TICK
#define LV_PM_TASK_TICK 10
#endif

typedef uintptr_t ui_msg_t;

#if defined(LV_PM_PLATFORM_RTTHREAD)
typedef rt_thread_t ui_thread_t;
typedef rt_mutex_t ui_mutex_t;
typedef rt_mailbox_t ui_queue_t;
typedef rt_tick_t ui_tick_t;
#elif defined(LV_PM_PLATFORM_ESP_IDF) || defined(LV_PM_PLATFORM_ARDUINO)
typedef TaskHandle_t ui_thread_t;
typedef SemaphoreHandle_t ui_mutex_t;
typedef QueueHandle_t ui_queue_t;
typedef TickType_t ui_tick_t;
#else
typedef void *ui_thread_t;
typedef void *ui_mutex_t;
typedef void *ui_queue_t;
typedef uint32_t ui_tick_t;
#endif

#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR >= 8)
#define UI_LVGL_HANDLER lv_timer_handler
#else
#define UI_LVGL_HANDLER lv_task_handler
#endif

#define UI_QUEUE_SIZE 10
#define UI_MSG_TYPE_SHIFT 16
#define UI_MSG_TYPE_MASK 0xffff0000UL
#define UI_MSG_DATA_MASK 0x0000ffffUL

typedef enum
{
    UI_MSG_NONE = 0,
    UI_MSG_PAGE_SWITCH = 1,
} ui_msg_type_t;

static rt_err_t platform_display_init(void);
static ui_queue_t platform_queue_create(void);
static rt_err_t platform_queue_send(ui_queue_t queue, ui_msg_t msg);
static rt_err_t platform_queue_recv(ui_queue_t queue, ui_msg_t *msg);
static ui_mutex_t platform_mutex_create(const char *name);
static void platform_mutex_lock(ui_mutex_t mutex);
static void platform_mutex_unlock(ui_mutex_t mutex);
static ui_thread_t platform_thread_create(void (*entry)(void *), void *parameter);
static void platform_thread_start(ui_thread_t thread);
static bool platform_is_current_thread(ui_thread_t thread);
static ui_tick_t platform_tick_get(void);
static uint32_t platform_tick_to_ms(ui_tick_t tick);
static void platform_delay_ms(uint32_t ms);

static void lvgl_task_entry(void *parameter);
static void menu_app_ui_init(void);
static rt_err_t switch_to_page_internal(PageType id);
static void page_on_enter_default(Page *page);
static void page_on_leave_default(Page *page);
static void page_on_destroy_default(Page *page);
static void start_page_on_create(Page *page);
static void gesture_event_cb(lv_event_t *e);
static void timer_manager_callback(lv_timer_t *timer);
static void process_ui_messages(void);

static bool is_valid_page_id(PageType id)
{
    return (int)id >= (int)PAGE_START && (int)id < (int)PAGE_SIZE;
}

#if defined(LV_PM_PLATFORM_RTTHREAD)
static rt_err_t platform_display_init(void)
{
#if defined(LV_PM_SKIP_DISPLAY_INIT)
    return RT_EOK;
#else
    return littlevgl2rtt_init("lcd");
#endif
}

static ui_queue_t platform_queue_create(void)
{
    return rt_mb_create("ui_mb", UI_QUEUE_SIZE, RT_IPC_FLAG_FIFO);
}

static rt_err_t platform_queue_send(ui_queue_t queue, ui_msg_t msg)
{
    return rt_mb_send(queue, (rt_ubase_t)msg);
}

static rt_err_t platform_queue_recv(ui_queue_t queue, ui_msg_t *msg)
{
    rt_ubase_t raw = 0;
    rt_err_t ret = rt_mb_recv(queue, &raw, 0);
    if (ret == RT_EOK && msg != RT_NULL)
    {
        *msg = (ui_msg_t)raw;
    }
    return ret;
}

static ui_mutex_t platform_mutex_create(const char *name)
{
    return rt_mutex_create(name, RT_IPC_FLAG_FIFO);
}

static void platform_mutex_lock(ui_mutex_t mutex)
{
    if (mutex != RT_NULL)
    {
        rt_mutex_take(mutex, RT_WAITING_FOREVER);
    }
}

static void platform_mutex_unlock(ui_mutex_t mutex)
{
    if (mutex != RT_NULL)
    {
        rt_mutex_release(mutex);
    }
}

static ui_thread_t platform_thread_create(void (*entry)(void *), void *parameter)
{
    return rt_thread_create("lvgl_task", entry, parameter, LV_PM_TASK_STACK_SIZE, LV_PM_TASK_PRIORITY, LV_PM_TASK_TICK);
}

static void platform_thread_start(ui_thread_t thread)
{
    if (thread != RT_NULL)
    {
        rt_thread_startup(thread);
    }
}

static bool platform_is_current_thread(ui_thread_t thread)
{
    return (thread != RT_NULL && rt_thread_self() == thread);
}

static ui_tick_t platform_tick_get(void)
{
    return rt_tick_get();
}

static uint32_t platform_tick_to_ms(ui_tick_t tick)
{
    return (uint32_t)(((uint64_t)tick * 1000ULL) / RT_TICK_PER_SECOND);
}

static void platform_delay_ms(uint32_t ms)
{
    rt_thread_mdelay(ms);
}

#elif defined(LV_PM_PLATFORM_ESP_IDF) || defined(LV_PM_PLATFORM_ARDUINO)
static rt_err_t platform_display_init(void)
{
#if defined(LV_PM_CALL_LV_INIT)
    lv_init();
#endif
    return RT_EOK;
}

static ui_queue_t platform_queue_create(void)
{
    return xQueueCreate(UI_QUEUE_SIZE, sizeof(ui_msg_t));
}

static rt_err_t platform_queue_send(ui_queue_t queue, ui_msg_t msg)
{
    return (queue != NULL && xQueueSend(queue, &msg, 0) == pdPASS) ? RT_EOK : -RT_ERROR;
}

static rt_err_t platform_queue_recv(ui_queue_t queue, ui_msg_t *msg)
{
    return (queue != NULL && msg != NULL && xQueueReceive(queue, msg, 0) == pdPASS) ? RT_EOK : -RT_ERROR;
}

static ui_mutex_t platform_mutex_create(const char *name)
{
    (void)name;
    return xSemaphoreCreateRecursiveMutex();
}

static void platform_mutex_lock(ui_mutex_t mutex)
{
    if (mutex != NULL)
    {
        xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
    }
}

static void platform_mutex_unlock(ui_mutex_t mutex)
{
    if (mutex != NULL)
    {
        xSemaphoreGiveRecursive(mutex);
    }
}

static ui_thread_t platform_thread_create(void (*entry)(void *), void *parameter)
{
    TaskHandle_t handle = NULL;
    BaseType_t ok = xTaskCreate(entry, "lvgl_task", LV_PM_TASK_STACK_SIZE, parameter, LV_PM_TASK_PRIORITY, &handle);
    return (ok == pdPASS) ? handle : NULL;
}

static void platform_thread_start(ui_thread_t thread)
{
    (void)thread;
}

static bool platform_is_current_thread(ui_thread_t thread)
{
    return (thread != NULL && xTaskGetCurrentTaskHandle() == thread);
}

static ui_tick_t platform_tick_get(void)
{
    return xTaskGetTickCount();
}

static uint32_t platform_tick_to_ms(ui_tick_t tick)
{
    return (uint32_t)pdTICKS_TO_MS(tick);
}

static void platform_delay_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
}

#else
static rt_err_t platform_display_init(void) { return RT_EOK; }
static ui_queue_t platform_queue_create(void) { return RT_NULL; }
static rt_err_t platform_queue_send(ui_queue_t queue, ui_msg_t msg)
{
    (void)queue;
    (void)msg;
    return -RT_ERROR;
}
static rt_err_t platform_queue_recv(ui_queue_t queue, ui_msg_t *msg)
{
    (void)queue;
    (void)msg;
    return -RT_ERROR;
}
static ui_mutex_t platform_mutex_create(const char *name)
{
    (void)name;
    return RT_NULL;
}
static void platform_mutex_lock(ui_mutex_t mutex) { (void)mutex; }
static void platform_mutex_unlock(ui_mutex_t mutex) { (void)mutex; }
static ui_thread_t platform_thread_create(void (*entry)(void *), void *parameter)
{
    (void)entry;
    (void)parameter;
    return RT_NULL;
}
static void platform_thread_start(ui_thread_t thread) { (void)thread; }
static bool platform_is_current_thread(ui_thread_t thread)
{
    (void)thread;
    return false;
}
static ui_tick_t platform_tick_get(void) { return 0; }
static uint32_t platform_tick_to_ms(ui_tick_t tick) { return tick; }
static void platform_delay_ms(uint32_t ms) { (void)ms; }
#endif

Page page_list[PAGE_SIZE] = {
    [PAGE_START] = {
        .id = PAGE_START,
        .app_id = APP_SIZE,
        .name = "Start Page",
        .timer_interval_ms = 0,
        .on_create = start_page_on_create,
        .is_created = false,
    },
};

static Page *current_page = RT_NULL;
static Page *last_page = RT_NULL;
static ui_thread_t lvgl_task = RT_NULL;
static ui_queue_t lvgl_mb = RT_NULL;
static ui_mutex_t lvgl_mutex = RT_NULL;
static ui_mutex_t page_mutex = RT_NULL;
static lv_style_t screen_style;
static device_switch_t device_switch = {0};

static bool is_lvgl_thread(void)
{
    return platform_is_current_thread(lvgl_task);
}

static void page_lock(void)
{
    if (page_mutex != RT_NULL)
    {
        platform_mutex_lock(page_mutex);
    }
}

static void page_unlock(void)
{
    if (page_mutex != RT_NULL)
    {
        platform_mutex_unlock(page_mutex);
    }
}

void ui_lock(void)
{
    if (lvgl_mutex != RT_NULL && !is_lvgl_thread())
    {
        platform_mutex_lock(lvgl_mutex);
    }
}

void ui_unlock(void)
{
    if (lvgl_mutex != RT_NULL && !is_lvgl_thread())
    {
        platform_mutex_unlock(lvgl_mutex);
    }
}

rt_err_t ui_init(void)
{
    if (lvgl_task != RT_NULL || lvgl_mb != RT_NULL ||
        lvgl_mutex != RT_NULL || page_mutex != RT_NULL)
    {
        return -RT_EBUSY;
    }

    rt_err_t ret = platform_display_init();
    if (ret != RT_EOK)
    {
        return ret;
    }

    lvgl_mb = platform_queue_create();
    if (lvgl_mb == RT_NULL)
    {
        return -RT_ERROR;
    }

    lvgl_mutex = platform_mutex_create("lvgl_mutex");
    if (lvgl_mutex == RT_NULL)
    {
        LV_PM_LOGE("Failed to create lvgl mutex");
        return -RT_ERROR;
    }

    page_mutex = platform_mutex_create("page_mutex");
    if (page_mutex == RT_NULL)
    {
        LV_PM_LOGE("Failed to create page mutex");
        return -RT_ERROR;
    }

    ui_lock();
    menu_app_ui_init();
    ui_unlock();

    lvgl_task = platform_thread_create(lvgl_task_entry, RT_NULL);
    if (lvgl_task == RT_NULL)
    {
        return -RT_ERROR;
    }

    platform_thread_start(lvgl_task);
    return RT_EOK;
}

static void lvgl_task_entry(void *parameter)
{
    (void)parameter;

    while (1)
    {
        platform_mutex_lock(lvgl_mutex);
        process_ui_messages();
        uint32_t delay_ms = UI_LVGL_HANDLER();
        platform_mutex_unlock(lvgl_mutex);

        if (delay_ms < 2)
        {
            delay_ms = 2;
        }
        else if (delay_ms > 20)
        {
            delay_ms = 20;
        }
        platform_delay_ms(delay_ms);
    }
}

static void process_ui_messages(void)
{
    if (lvgl_mb == RT_NULL)
    {
        return;
    }

    ui_msg_t msg = 0;
    while (platform_queue_recv(lvgl_mb, &msg) == RT_EOK)
    {
        ui_msg_type_t type = (ui_msg_type_t)((msg & UI_MSG_TYPE_MASK) >> UI_MSG_TYPE_SHIFT);
        uint16_t data = (uint16_t)(msg & UI_MSG_DATA_MASK);

        switch (type)
        {
        case UI_MSG_PAGE_SWITCH:
            switch_to_page_internal((PageType)data);
            break;
        default:
            break;
        }
    }
}

static void menu_app_ui_init(void)
{
    lv_style_init(&screen_style);
    lv_style_set_size(&screen_style, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_style_set_border_width(&screen_style, 0);
    lv_style_set_pad_all(&screen_style, 0);
    lv_style_set_opa(&screen_style, LV_OPA_COVER);
    lv_style_set_bg_color(&screen_style, lv_color_hex(LV_COLOR_THEME_BLACK));
    lv_style_set_text_color(&screen_style, lv_color_hex(LV_COLOR_THEME_WHITE));

    current_page = &page_list[PAGE_START];
    last_page = &page_list[PAGE_START];

    lv_timer_create(timer_manager_callback, 10, NULL);
    page_on_enter_default(&page_list[PAGE_START]);
}

static void timer_manager_callback(lv_timer_t *timer)
{
    static ui_tick_t last_tick = 0;
    ui_tick_t current_tick = platform_tick_get();

    if (last_tick == 0)
    {
        last_tick = current_tick;
        return;
    }

    uint32_t elapsed_ms = platform_tick_to_ms(current_tick - last_tick);
    if (elapsed_ms < 10)
    {
        return;
    }
    last_tick = current_tick;

    Page *page = RT_NULL;
    bool should_fire = false;

    page_lock();
    page = current_page;
    uint32_t interval_ms = 0;
    if (page != RT_NULL)
    {
        interval_ms = page->timer_interval_ms ? page->timer_interval_ms : page->timer_interval;
    }

    if (page != RT_NULL && page->page_timer != RT_NULL && interval_ms > 0 && page->is_created)
    {
        page->timer_counter_ms += elapsed_ms;
        page->timer_counter = page->timer_counter_ms;
        if (page->timer_counter_ms >= interval_ms)
        {
            page->timer_counter_ms = 0;
            page->timer_counter = 0;
            should_fire = true;
        }
    }
    page_unlock();

    if (should_fire && page != RT_NULL && page == current_page)
    {
        page->page_timer(page);
    }

    (void)timer;
}

static void start_page_on_create(Page *page)
{
    LV_PM_LOGD("start_page_on_create: %d", page ? page->id : -1);
}

rt_err_t ui_page_register(const Page *page)
{
    if (page == RT_NULL || !is_valid_page_id(page->id))
    {
        return -RT_EINVAL;
    }

    page_lock();
    page_list[page->id] = *page;
    page_list[page->id].root = RT_NULL;
    page_list[page->id].group = RT_NULL;
    page_list[page->id].timer = RT_NULL;
    page_list[page->id].last_root = RT_NULL;
    page_list[page->id].last_group_obj = RT_NULL;
    page_list[page->id].is_created = false;
    page_list[page->id].timer_counter_ms = 0;
    page_list[page->id].timer_counter = 0;
    page_unlock();

    return RT_EOK;
}

Page *ui_page_get(PageType id)
{
    if (!is_valid_page_id(id))
    {
        return RT_NULL;
    }
    return &page_list[id];
}

Page *ui_page_get_current(void)
{
    return current_page;
}

rt_err_t ui_page_switch(PageType id)
{
    rt_err_t ret;

    ui_lock();
    ret = switch_to_page_internal(id);
    ui_unlock();

    return ret;
}

rt_err_t ui_page_switch_async(PageType id)
{
    if (!is_valid_page_id(id) || lvgl_mb == RT_NULL)
    {
        return -RT_EINVAL;
    }

    ui_msg_t msg = (((ui_msg_t)UI_MSG_PAGE_SWITCH) << UI_MSG_TYPE_SHIFT) | (ui_msg_t)id;
    return platform_queue_send(lvgl_mb, msg);
}

rt_err_t ui_page_destroy(PageType id)
{
    if (!is_valid_page_id(id))
    {
        return -RT_EINVAL;
    }

    ui_lock();
    Page *page = &page_list[id];
    if (page == current_page)
    {
        ui_unlock();
        return -RT_EBUSY;
    }

    page_on_destroy_default(page);
    ui_unlock();

    return RT_EOK;
}

static void page_on_enter_default(Page *page)
{
    if (page == RT_NULL)
    {
        return;
    }

    LV_PM_LOGD("page_on_enter: %d", page->id);

    if (!page->is_created)
    {
        page->root = lv_obj_create(NULL);
        if (page->root == RT_NULL)
        {
            LV_PM_LOGE("Failed to create page root: %d", page->id);
            return;
        }

        lv_obj_add_style(page->root, &screen_style, 0);
        lv_obj_add_flag(page->root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(page->root, gesture_event_cb, LV_EVENT_GESTURE, (void *)(uintptr_t)page->id);

        page->group = lv_group_create();
        if (page->group != RT_NULL)
        {
            lv_group_set_wrap(page->group, false);
            if (page->group_cb != RT_NULL)
            {
                lv_group_set_focus_cb(page->group, page->group_cb);
            }
        }

        if (page->on_create != RT_NULL)
        {
            page->on_create(page);
        }

        page->is_created = true;
        page->timer_counter_ms = 0;
        page->timer_counter = 0;
    }

    if (page->id == PAGE_START)
    {
        lv_screen_load(page->root);
    }
    // else if( page->id == PAGE_HOME)
    // {
    //     lv_screen_load_anim(page->root, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    // }
    else
    {
        // lv_screen_load_anim(page->root, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
        lv_screen_load(page->root);
    }

    if (page->on_enter != RT_NULL)
    {
        page->on_enter(page);
    }
}

static void page_on_leave_default(Page *page)
{
    if (page == RT_NULL)
    {
        return;
    }

    LV_PM_LOGD("page_on_leave: %d", page->id);

    if (page->group != RT_NULL)
    {
        page->last_group_obj = lv_group_get_focused(page->group);
    }

    page->timer_counter_ms = 0;
    page->timer_counter = 0;

    if (page->on_leave != RT_NULL)
    {
        page->on_leave(page);
    }
}

static void page_on_destroy_default(Page *page)
{
    if (page == RT_NULL)
    {
        return;
    }

    LV_PM_LOGD("page_on_destroy: %d", page->id);

    if (page->on_destroy != RT_NULL && page->on_destroy != page_on_destroy_default)
    {
        page->on_destroy(page);
    }

    if (page->timer != RT_NULL)
    {
        lv_timer_del(page->timer);
        page->timer = RT_NULL;
    }

    if (page->group != RT_NULL)
    {
        lv_group_del(page->group);
        page->group = RT_NULL;
    }

    if (page->root != RT_NULL)
    {
#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR >= 9)
        lv_obj_delete(page->root);
#else
        lv_obj_del(page->root);
#endif
        page->root = RT_NULL;
    }

    page->is_created = false;
    page->timer_counter_ms = 0;
    page->timer_counter = 0;
}

static rt_err_t switch_to_page_internal(PageType id)
{
    if (!is_valid_page_id(id))
    {
        return -RT_EINVAL;
    }

    Page *next = &page_list[id];
    if (next->id != id || next->id >= PAGE_SIZE)
    {
        return -RT_EINVAL;
    }

    page_lock();

    if (current_page == next)
    {
        page_unlock();
        return RT_EOK;
    }

    Page *prev = current_page;
    last_page = prev;
    current_page = next;

    LV_PM_LOGD("switch_to_page: %d -> %d", prev ? prev->id : -1, next->id);

    page_unlock();

    if (prev != RT_NULL)
    {
        page_on_leave_default(prev);
    }

    page_on_enter_default(next);
    return RT_EOK;
}

static GestureDirection gesture_from_lvgl(lv_event_t *e)
{
// #if defined(LV_DIR_LEFT)
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == RT_NULL)
    {
        indev = lv_indev_active();
    }
    if (indev == RT_NULL)
    {
        return GESTURE_NONE;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT)
    {
        return GESTURE_LEFT;
    }
    if (dir == LV_DIR_RIGHT)
    {
        return GESTURE_RIGHT;
    }
    if (dir == LV_DIR_TOP)
    {
        return GESTURE_UP;
    }
    if (dir == LV_DIR_BOTTOM)
    {
        return GESTURE_DOWN;
    }
// #endif
    return GESTURE_NONE;
}

static bool gesture_started_on_interactive_object(lv_obj_t *page_root)
{
    lv_obj_t *active = lv_indev_get_active_obj();
    while (active != RT_NULL && active != page_root)
    {
        if (lv_obj_has_flag(active, LV_OBJ_FLAG_CLICKABLE) ||
            lv_obj_has_flag(active, LV_OBJ_FLAG_SCROLLABLE))
        {
            return true;
        }
        active = lv_obj_get_parent(active);
    }
    return false;
}

static void gesture_event_cb(lv_event_t *e)
{
    uintptr_t id = (uintptr_t)lv_event_get_user_data(e);
    Page *page = ui_page_get((PageType)id);
    if (page == RT_NULL || page->on_gesture == RT_NULL)
    {
        return;
    }

#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR >= 9)
    lv_obj_t *target = lv_event_get_target_obj(e);
#else
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
#endif
    if (target != RT_NULL && target != page->root &&
        (lv_obj_has_flag(target, LV_OBJ_FLAG_CLICKABLE) ||
         lv_obj_has_flag(target, LV_OBJ_FLAG_SCROLLABLE)))
    {
        return;
    }

    if (gesture_started_on_interactive_object(page->root))
    {
        return;
    }

    GestureDirection direction = gesture_from_lvgl(e);
    if (direction != GESTURE_NONE)
    {
        page->on_gesture(page, direction);
    }
}

device_switch_t *get_device_switch(void)
{
    return &device_switch;
}
