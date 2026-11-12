#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool enabled;
    bool connected;
    bool rssi_valid;
    int rssi_dbm;
    char advertised_name[32];
    char local_address[24];
    char host_identifier[64];
} bluetooth_control_info_t;

bool bluetooth_control_is_enabled(void);
bool bluetooth_control_is_connected(void);
esp_err_t bluetooth_control_get_status(char *buffer, size_t buffer_size);
esp_err_t bluetooth_control_set_enabled(bool enabled);
esp_err_t bluetooth_control_get_info(bluetooth_control_info_t *info);

#ifdef __cplusplus
}
#endif
