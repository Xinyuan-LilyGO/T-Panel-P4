#pragma once

#include <stdbool.h>

#include <esp_err.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool wifi_connected;
    bool bluetooth_enabled;
    bool bluetooth_connected;
    bool battery_charging;
    int battery_percent;
    int wifi_rssi_dbm;
} status_info_t;

typedef void (*status_bar_status_provider_t)(
    status_info_t *status,
    void *context);

void status_bar_create(void);
void status_bar_update(const status_info_t *status);
void status_bar_start_monitor(
    status_bar_status_provider_t provider,
    void *context);
esp_err_t status_bar_stop_monitor(void);
lv_obj_t *status_bar_get_container(void);

#ifdef __cplusplus
}
#endif
