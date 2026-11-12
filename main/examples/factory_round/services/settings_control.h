#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SETTINGS_POWER_ROW_COUNT = 19, SETTINGS_POWER_TEXT_SIZE = 1024 };
esp_err_t settings_control_read_power(char *buffer, size_t buffer_size);
esp_err_t settings_control_get_charger_enabled(bool *enabled);
esp_err_t settings_control_set_charger_enabled(bool enabled);
esp_err_t settings_control_get_backlight_brightness(uint8_t *percent);
esp_err_t settings_control_set_backlight_brightness(uint8_t percent);
esp_err_t settings_control_get_audio_volume(uint8_t *percent);
esp_err_t settings_control_set_audio_volume(uint8_t percent);
bool settings_control_is_sd_msc_enabled(void);
esp_err_t settings_control_set_sd_msc_enabled(bool enabled);
esp_err_t settings_control_read_sd_info(char *buffer, size_t buffer_size);
esp_err_t settings_control_run_check(char *buffer, size_t buffer_size);
esp_err_t settings_control_enter_ship_mode(void);
void settings_ship_progress_reset(void);
void settings_ship_progress_update(uint8_t step, bool complete, esp_err_t result);
void settings_sleep_progress_reset(uint8_t mode);
void settings_sleep_progress_update(uint8_t step, bool complete, esp_err_t result);
esp_err_t settings_control_enter_deep_sleep(void);
esp_err_t settings_control_enter_light_sleep(void);

#ifdef __cplusplus
}
#endif
