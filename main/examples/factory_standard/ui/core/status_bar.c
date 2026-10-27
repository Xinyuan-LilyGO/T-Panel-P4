#include "status_bar.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <ui/core/ui.h>

#define STATUS_BAR_HEIGHT 48
#define STATUS_BAR_HORIZONTAL_PADDING 32
#define STATUS_BAR_ITEM_GAP 14

static const char *const STATUS_BAR_TAG = "factory_status";
static const char *const STATUS_BAR_SNTP_SERVER = "pool.ntp.org";
static const char *const STATUS_BAR_TIME_ZONE = "CST-8";

typedef struct
{
    lv_obj_t *container;
    lv_obj_t *clock_group;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_obj_t *wifi_icon;
    lv_obj_t *bluetooth_icon;
    lv_obj_t *battery_icon;
    lv_obj_t *battery_label;
    lv_obj_t *battery_bar;
    lv_obj_t *wifi_bars[4];
} status_bar_ui_t;

static status_bar_ui_t s_status_ui;
static status_info_t s_status_info = {
    .battery_percent = -1,
};
static bool s_time_sync_attempted;
static status_bar_status_provider_t s_status_provider;
static void *s_status_provider_context;
static bool s_status_monitor_started;
static TaskHandle_t s_status_monitor_task;
static SemaphoreHandle_t s_status_monitor_stopped;
static lv_timer_t *s_battery_charge_timer;
static int s_battery_charge_value;
static bool s_battery_charge_animating;

static void status_bar_battery_charge_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_battery_charge_animating || s_status_ui.battery_bar == NULL)
        return;

    s_battery_charge_value += 4;
    if (s_battery_charge_value > 100)
        s_battery_charge_value = 0;
    lv_bar_set_value(s_status_ui.battery_bar, s_battery_charge_value, LV_ANIM_OFF);
}

static void status_bar_monitor_task(void *argument)
{
    (void)argument;
    while (true)
    {
        status_info_t status = {
            .battery_percent = -1,
        };
        if (s_status_provider != NULL)
            s_status_provider(&status, s_status_provider_context);
        status_bar_update(&status);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) != 0U)
            break;
    }

    s_status_provider = NULL;
    s_status_provider_context = NULL;
    s_status_monitor_task = NULL;
    s_status_monitor_started = false;
    if (s_status_monitor_stopped != NULL)
        xSemaphoreGive(s_status_monitor_stopped);
    vTaskDelete(NULL);
}

static void status_bar_time_sync_task(void *argument)
{
    (void)argument;
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG(STATUS_BAR_SNTP_SERVER);
    esp_err_t result = esp_netif_sntp_init(&sntp_config);
    if (result == ESP_OK)
    {
        result = ESP_ERR_TIMEOUT;
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
            if (result == ESP_OK)
                break;
            if (result != ESP_ERR_TIMEOUT && result != ESP_ERR_NOT_FINISHED)
                break;
        }
    }

    if (result == ESP_OK)
    {
        if (setenv("TZ", STATUS_BAR_TIME_ZONE, 1) == 0)
        {
            tzset();
            ESP_LOGI(STATUS_BAR_TAG, "SNTP time synchronized");
        }
        else
        {
            result = ESP_FAIL;
        }
    }
    if (result != ESP_OK)
        ESP_LOGW(STATUS_BAR_TAG, "SNTP synchronization failed: %s", esp_err_to_name(result));

    esp_netif_sntp_deinit();
    vTaskDelete(NULL);
}

static void status_bar_start_time_sync(void)
{
    if (!s_status_info.wifi_connected || s_time_sync_attempted)
        return;
    // Attempt at most once per boot. Repeated SNTP timeouts create recurring
    // network work that can interfere with real-time audio playback.
    s_time_sync_attempted = true;
    if (xTaskCreate(
            status_bar_time_sync_task,
            "status_sntp",
            2048,
            NULL,
            3,
            NULL) != pdPASS)
    {
        ESP_LOGW(STATUS_BAR_TAG, "SNTP task creation failed");
    }
}

static int status_bar_clamp_percent(int percent)
{
    if (percent < 0)
    {
        return -1;
    }
    return percent > 100 ? 100 : percent;
}

static lv_obj_t *status_bar_label_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_ShareTechMono1_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    return label;
}

static lv_obj_t *status_bar_wifi_create(lv_obj_t *parent)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_set_size(icon, 40, 40);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_remove_flag(icon, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));

    const lv_coord_t heights[] = {16, 24, 32, 40};
    for (size_t index = 0; index < 4U; ++index)
    {
        lv_obj_t *bar = lv_obj_create(icon);
        lv_obj_set_pos(bar, (lv_coord_t)(index * 10), 40 - heights[index]);
        lv_obj_set_size(bar, 8, heights[index]);
        lv_obj_set_style_bg_opa(bar, index < 2U ? LV_OPA_100 : LV_OPA_20, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(CYBER_YELLOW_GREEN), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        s_status_ui.wifi_bars[index] = bar;
    }
    return icon;
}

static lv_obj_t *status_bar_spacer_create(lv_obj_t *parent)
{
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    return spacer;
}

static void status_bar_refresh(void)
{
    char time_text[6] = "--:--";
    char date_text[11] = "----/--/--";
    const time_t now = time(NULL);
    struct tm time_info;
    if (now > 1609459200 && localtime_r(&now, &time_info) != NULL)
    {
        strftime(time_text, sizeof(time_text), "%H:%M", &time_info);
        strftime(date_text, sizeof(date_text), "%Y/%m/%d", &time_info);
    }

    lv_label_set_text(s_status_ui.time_label, time_text);
    lv_label_set_text(s_status_ui.date_label, date_text);
    lv_obj_set_style_text_color(
        s_status_ui.wifi_icon,
        lv_color_hex(CYBER_YELLOW_GREEN),
        0);
    int wifi_level = 0;
    if (s_status_info.wifi_connected && s_status_info.wifi_rssi_dbm >= -85)
        wifi_level = s_status_info.wifi_rssi_dbm >= -55 ? 4 : s_status_info.wifi_rssi_dbm >= -67 ? 3
                                                          : s_status_info.wifi_rssi_dbm >= -75   ? 2
                                                                                                 : 1;
    for (int index = 0; index < 4; ++index)
        lv_obj_set_style_bg_opa(s_status_ui.wifi_bars[index],
                                index < wifi_level ? LV_OPA_COVER : 21, 0);

    uint32_t bluetooth_color = CYBER_YELLOW_GREEN;
    lv_obj_set_style_text_color(
        s_status_ui.bluetooth_icon, lv_color_hex(bluetooth_color), 0);
    lv_obj_set_style_text_opa(
        s_status_ui.bluetooth_icon,
        s_status_info.bluetooth_enabled && s_status_info.bluetooth_connected
            ? LV_OPA_COVER
            : 21,
        0);

    const int battery_percent =
        status_bar_clamp_percent(s_status_info.battery_percent);
    const uint32_t battery_color =
        battery_percent >= 0 && battery_percent < 20
            ? CYBER_MAGENTA
            : CYBER_YELLOW_GREEN;
    if (battery_percent >= 0)
    {
        lv_label_set_text_fmt(
            s_status_ui.battery_label, "%d%%", battery_percent);
    }
    else
    {
        lv_label_set_text(s_status_ui.battery_label, "--%");
    }
    lv_obj_set_style_text_color(
        s_status_ui.battery_label, lv_color_hex(battery_color), 0);
    lv_obj_set_style_bg_color(
        s_status_ui.battery_bar, lv_color_hex(battery_color), 0);
    lv_obj_set_style_bg_color(
        s_status_ui.battery_bar, lv_color_hex(battery_color), LV_PART_INDICATOR);

    // Do not start an indeterminate charging animation when the gauge read
    // failed.  The provider can still obtain a transient power-state sample
    // while the battery percentage is unavailable during attach/detach.
    if (s_status_info.battery_charging && battery_percent >= 0 &&
        s_status_ui.battery_bar != NULL)
    {
        if (!s_battery_charge_animating)
        {
            s_battery_charge_animating = true;
            s_battery_charge_value = 0;
            lv_bar_set_value(s_status_ui.battery_bar, 0, LV_ANIM_OFF);
            if (s_battery_charge_timer != NULL)
                lv_timer_resume(s_battery_charge_timer);
        }
    }
    else
    {
        if (s_battery_charge_animating)
        {
            s_battery_charge_animating = false;
            if (s_battery_charge_timer != NULL)
                lv_timer_pause(s_battery_charge_timer);
        }
        lv_bar_set_value(s_status_ui.battery_bar,
                         battery_percent >= 0 ? battery_percent : 0, LV_ANIM_OFF);
    }
}

void status_bar_create(void)
{
    ui_lock();
    if (s_status_ui.container != NULL)
    {
        lv_obj_move_foreground(s_status_ui.container);
        ui_unlock();
        return;
    }

    s_status_ui.container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_status_ui.container, SCREEN_WIDTH, STATUS_BAR_HEIGHT);
    lv_obj_align(s_status_ui.container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_status_ui.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_status_ui.container, 0, 0);
    lv_obj_set_style_radius(s_status_ui.container, 0, 0);
    lv_obj_set_style_pad_all(s_status_ui.container, 0, 0);
    lv_obj_remove_flag(s_status_ui.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_status_ui.container, LV_OBJ_FLAG_CLICKABLE);

    s_status_ui.time_label = status_bar_label_create(s_status_ui.container, "--:--");
    lv_obj_set_pos(s_status_ui.time_label, 64, 0);
    lv_obj_set_size(s_status_ui.time_label, 100, 28);
    lv_obj_set_style_text_font(s_status_ui.time_label, &lv_font_ShareTechMono1_28, 0);
    lv_obj_set_style_text_opa(s_status_ui.time_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_status_ui.time_label, lv_color_hex(CYBER_MAGENTA), 0);
    s_status_ui.date_label = status_bar_label_create(s_status_ui.container, "----/--/--");
    lv_obj_set_pos(s_status_ui.date_label, 64, 28);
    lv_obj_set_size(s_status_ui.date_label, 117, 20);
    lv_obj_set_style_text_font(s_status_ui.date_label, &lv_font_ShareTechMono1_16, 0);
    lv_obj_set_style_text_opa(s_status_ui.date_label, 171, 0);
    s_status_ui.wifi_icon = status_bar_wifi_create(s_status_ui.container);
    lv_obj_set_pos(s_status_ui.wifi_icon, 538, 4);
    s_status_ui.bluetooth_icon =
        status_bar_label_create(s_status_ui.container, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_pos(s_status_ui.bluetooth_icon, 493, 12);
    lv_obj_set_size(s_status_ui.bluetooth_icon, 36, 36);
    lv_obj_set_style_text_font(s_status_ui.bluetooth_icon, &lv_font_ShareTechMono1_32, 0);
    lv_obj_set_style_text_align(s_status_ui.bluetooth_icon, LV_TEXT_ALIGN_CENTER, 0);
    s_status_ui.battery_icon =
        NULL;
    s_status_ui.battery_label =
        status_bar_label_create(s_status_ui.container, "--%");
    lv_obj_set_pos(s_status_ui.battery_label, 590, 0);
    lv_obj_set_size(s_status_ui.battery_label, 60, 24);
    lv_obj_set_style_text_font(s_status_ui.battery_label, &lv_font_ShareTechMono1_24, 0);
    lv_obj_set_style_text_color(s_status_ui.battery_label, lv_color_hex(CYBER_YELLOW_GREEN), 0);

    s_status_ui.battery_bar = lv_bar_create(s_status_ui.container);
    lv_obj_set_pos(s_status_ui.battery_bar, 590, 30);
    lv_obj_set_size(s_status_ui.battery_bar, 60, 16);
    lv_bar_set_range(s_status_ui.battery_bar, 0, 100);
    lv_obj_set_style_bg_opa(s_status_ui.battery_bar, 60, 0);
    lv_obj_set_style_bg_color(s_status_ui.battery_bar, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_radius(s_status_ui.battery_bar, 0, 0);
    lv_obj_set_style_bg_color(s_status_ui.battery_bar, lv_color_hex(CYBER_YELLOW_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_status_ui.battery_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_status_ui.battery_bar, 0, LV_PART_INDICATOR);

    s_battery_charge_timer = lv_timer_create(
        status_bar_battery_charge_timer_cb, 50, NULL);
    lv_timer_pause(s_battery_charge_timer);

    status_bar_refresh();
    ui_unlock();
}

void status_bar_update(const status_info_t *status)
{
    ui_lock();
    if (status != NULL)
    {
        s_status_info = *status;
        status_bar_start_time_sync();
    }
    if (s_status_ui.container != NULL)
    {
        status_bar_refresh();
    }
    ui_unlock();
}

void status_bar_start_monitor(
    status_bar_status_provider_t provider,
    void *context)
{
    if (provider == NULL || s_status_monitor_started)
        return;
    if (s_status_monitor_stopped == NULL)
        s_status_monitor_stopped = xSemaphoreCreateBinary();
    if (s_status_monitor_stopped == NULL)
    {
        ESP_LOGE(STATUS_BAR_TAG, "status monitor stop semaphore creation failed");
        return;
    }
    (void)xSemaphoreTake(s_status_monitor_stopped, 0);
    s_status_provider = provider;
    s_status_provider_context = context;
    s_status_monitor_started = true;
    if (xTaskCreate(
            status_bar_monitor_task,
            "factory_status",
            2048,
            NULL,
            4,
            &s_status_monitor_task) != pdPASS)
    {
        s_status_monitor_started = false;
        s_status_monitor_task = NULL;
        s_status_provider = NULL;
        s_status_provider_context = NULL;
        ESP_LOGE(STATUS_BAR_TAG, "status monitor task creation failed");
    }
}

esp_err_t status_bar_stop_monitor(void)
{
    if (!s_status_monitor_started || s_status_monitor_task == NULL)
        return ESP_OK;
    if (s_status_monitor_stopped == NULL)
        return ESP_ERR_INVALID_STATE;

    xTaskNotifyGive(s_status_monitor_task);
    if (xSemaphoreTake(s_status_monitor_stopped, portMAX_DELAY) != pdTRUE)
        return ESP_FAIL;
    return ESP_OK;
}

lv_obj_t *status_bar_get_container(void)
{
    return s_status_ui.container;
}
