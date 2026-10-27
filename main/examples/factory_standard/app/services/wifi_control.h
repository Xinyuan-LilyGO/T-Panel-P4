#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wifi_control_is_enabled(void);
esp_err_t wifi_control_get_status(char *buffer, size_t buffer_size);
esp_err_t wifi_control_set_enabled(bool enabled);
esp_err_t wifi_control_scan(char *buffer, size_t buffer_size);
esp_err_t wifi_control_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
