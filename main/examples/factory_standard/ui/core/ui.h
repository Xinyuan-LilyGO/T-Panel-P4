#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>
#include <lvgl_page_manager.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CYBER_BG 0x0D0D0D
#define CYBER_PANEL 0x10191A
#define CYBER_GREEN 0x00FF00
#define CYBER_MAGENTA 0xFF006E
#define CYBER_CYAN 0x00E5FF
#define CYBER_YELLOW_GREEN 0xC8FF00
#define CYBER_YELLOW 0xffff00
#define CYBER_TEXT 0xE8FFFF
#define CYBER_MUTED 0x689494
#define CYBER_GRID 0x153C3C
/* Keep the shared page-manager palette aligned with the factory cyber theme. */
#undef UI_BG
#undef UI_PANEL
#undef UI_PANEL_HL
#undef UI_LINE
#undef UI_TEXT
#undef UI_MUTED
#undef UI_PRIMARY
#undef UI_SECONDARY
#undef UI_WARN
#undef UI_OK
#undef UI_ERROR
#define UI_BG CYBER_BG
#define UI_PANEL CYBER_PANEL
#define UI_PANEL_HL CYBER_PANEL
#define UI_LINE CYBER_GRID
#define UI_TEXT CYBER_TEXT
#define UI_MUTED CYBER_MUTED
#define UI_PRIMARY CYBER_CYAN
#define UI_SECONDARY CYBER_GREEN
#define UI_WARN CYBER_YELLOW
#define UI_OK CYBER_GREEN
#define UI_ERROR CYBER_MAGENTA

LV_FONT_DECLARE(lv_font_ShareTechMono1_16);
LV_FONT_DECLARE(lv_font_ShareTechMono1_20);
LV_FONT_DECLARE(lv_font_ShareTechMono1_24);
LV_FONT_DECLARE(lv_font_ShareTechMono1_28);
LV_FONT_DECLARE(lv_font_ShareTechMono1_32);
LV_FONT_DECLARE(lv_font_ShareTechMono1_50);
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

esp_err_t factory_ui_start(void);
/* Settings actions are implemented by the board owning application. */
esp_err_t settings_control_read_power(char *buffer, size_t buffer_size);
esp_err_t settings_control_get_backlight_brightness(uint8_t *percent);
esp_err_t settings_control_set_backlight_brightness(uint8_t percent);
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
