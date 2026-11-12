#include "page_common.hpp"
#include "page_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <services/wifi_control.h>
#include <services/ble_control.h>
#include <services/settings_control.h>

namespace factory::ui
{
    namespace
    {
        constexpr lv_coord_t kPanelWidth = 480;
        // A centered 480x480 panel (including its halo) fits inside 720x720 round glass.
        constexpr lv_coord_t kPopupSize = 480;
        constexpr lv_coord_t kPopupContentWidth = kPopupSize - 48;
        constexpr lv_coord_t kPopupActionY = 414;
        constexpr lv_coord_t kCardWidth = 228;
        constexpr lv_coord_t kCardHeight = 92;
        constexpr lv_coord_t kColumnGap = 24;
        constexpr lv_coord_t kRowGap = 18;
        constexpr uint32_t kActiveColor = 0x4c6edb;
        constexpr uint32_t kMutedColor = 0x8d92b7;

        struct SetUi
        {
            lv_obj_t *root{};
            lv_obj_t *cards[6]{};
        };

        SetUi s_set;

        lv_obj_t *setLabel(lv_obj_t *parent, const char *text,
                           const lv_font_t *font, uint32_t color)
        {
            return page_ui_label(parent, text, font, color);
        }

        void stylePanel(lv_obj_t *object, lv_coord_t radius = 22)
        {
            if (object == nullptr)
                return;
            page_ui_apply_icon_style(object);
            lv_obj_set_style_radius(object, radius, LV_PART_MAIN);
            lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_t *s_settings_menu{};
        lv_obj_t *s_flow_scroll{};
        lv_obj_t *s_check_scroll{};

        lv_obj_t *settings_ui_button(lv_obj_t *parent, const char *, const char *text,
                                     lv_coord_t width, lv_coord_t height,
                                     lv_event_cb_t callback, void *data)
        {
            lv_obj_t *button = lv_obj_create(parent);
            lv_obj_set_size(button, width, height);
            stylePanel(button, 18);
            lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(button, lv_color_hex(kActiveColor), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
            if (callback != nullptr)
                lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
            lv_obj_t *label = setLabel(button, text, &lv_font_montserrat_16, UI_WIHTE);
            lv_obj_center(label);
            return button;
        }

        typedef enum
        {
            SETTINGS_ACTION_NONE,
            SETTINGS_ACTION_SD_MSC,
            SETTINGS_ACTION_SHIP,
            SETTINGS_ACTION_DEEP_SLEEP,
            SETTINGS_ACTION_LIGHT_SLEEP,
            SETTINGS_ACTION_CHECK,
        } settings_action_t;

        static lv_obj_t *s_settings_popup;
        static lv_obj_t *s_settings_popup_body;
        static lv_obj_t *s_settings_popup_title;
        static lv_obj_t *s_wifi_status;
        static lv_timer_t *s_wifi_status_timer;
        static lv_obj_t *s_bluetooth_status;
        static lv_timer_t *s_bluetooth_status_timer;
        static lv_obj_t *s_bluetooth_info_names[5];
        static lv_obj_t *s_bluetooth_info_values[5];
        static lv_obj_t *s_bluetooth_info_lines[5];
        static lv_obj_t *s_settings_popup_message;
        static lv_obj_t *s_settings_popup_yes;
        static lv_obj_t *s_settings_popup_no;
        static lv_obj_t *s_settings_popup_alt;
        static lv_obj_t *s_settings_popup_x;
        static lv_obj_t *s_level_slider;
        static lv_obj_t *s_level_value;
        static lv_timer_t *s_monitor_timer;
        static lv_obj_t *s_monitor_scroll;
        static lv_obj_t *s_monitor_lines[SETTINGS_POWER_ROW_COUNT];
        static lv_obj_t *s_monitor_names[SETTINGS_POWER_ROW_COUNT];
        static lv_obj_t *s_monitor_values[SETTINGS_POWER_ROW_COUNT];
        static lv_obj_t *s_charger_toggle;
        static lv_obj_t *s_check_lines[9];
        static lv_obj_t *s_check_names[9];
        static lv_obj_t *s_check_values[9];
        static lv_obj_t *s_sd_info_names[8];
        static lv_obj_t *s_sd_info_values[8];
        static lv_obj_t *s_sd_msc_toggle;
        static settings_action_t s_settings_action;
        static lv_obj_t *s_wifi_toggle;
        static lv_obj_t *s_bluetooth_toggle;
        static lv_obj_t *s_wifi_scan;
        static lv_obj_t *s_wifi_connect;
        static lv_obj_t *s_wifi_ssid;
        static lv_obj_t *s_wifi_password;
        static lv_obj_t *s_wifi_keyboard;
        static lv_obj_t *s_wifi_results;
        static lv_obj_t *s_wifi_close;
        static constexpr std::uint8_t kShipUiCount = 7U;
        static constexpr std::uint8_t kFlowUiCapacity = 10U;
        static constexpr std::uint8_t kShipUiStepIds[kShipUiCount] = {1U, 2U, 4U, 5U, 6U, 3U, 9U};
        static lv_obj_t *s_ship_steps[kFlowUiCapacity];
        static lv_obj_t *s_ship_names[kFlowUiCapacity];
        static lv_obj_t *s_ship_lines[kFlowUiCapacity];
        static bool s_ship_running;
        static lv_timer_t *s_ship_refresh_timer;
        static std::atomic<std::uint8_t> s_ship_state[kFlowUiCapacity];
        static std::atomic<int> s_ship_result[kFlowUiCapacity];
        static std::atomic<bool> s_ship_done;
        static std::atomic<int> s_ship_overall_result;
        static std::uint8_t s_flow_count = kShipUiCount;
        static char s_wifi_scan_text[480];
        static char s_wifi_ssids[16][33];
        static size_t s_wifi_ssid_count;
        static std::atomic<bool> s_wifi_scan_done;
        static std::atomic<int> s_wifi_scan_result;
        static std::atomic<bool> s_wifi_scan_running;
        static lv_timer_t *s_wifi_scan_timer;
        static constexpr std::size_t kCheckUiCount = 9U;
        static lv_obj_t *settings_wifi_result_button(const char *ssid, lv_coord_t y);
        static void settings_bluetooth_info_refresh(void);

        static void settings_wifi_scan_worker(void *)
        {
            s_wifi_scan_result.store(wifi_control_scan(s_wifi_scan_text, sizeof(s_wifi_scan_text)));
            s_wifi_scan_done.store(true);
            vTaskDelete(NULL);
        }

        static void settings_wifi_scan_refresh(lv_timer_t *timer)
        {
            (void)timer;
            if (!s_wifi_scan_done.exchange(false))
                return;
            s_wifi_scan_running.store(false);
            lv_timer_pause(s_wifi_scan_timer);
            lv_obj_clear_state(s_wifi_toggle, LV_STATE_DISABLED);
            lv_obj_clear_state(s_wifi_scan, LV_STATE_DISABLED);
            lv_obj_clear_state(s_wifi_connect, LV_STATE_DISABLED);
            lv_label_set_text(lv_obj_get_child(s_wifi_toggle, 0),
                              wifi_control_is_enabled() ? "WIFI OFF" : "WIFI ON");
            char status_text[96];
            if (wifi_control_get_status(status_text, sizeof(status_text)) == ESP_OK)
                lv_label_set_text(s_wifi_status, status_text);
            const esp_err_t result = static_cast<esp_err_t>(s_wifi_scan_result.load());
            if (result != ESP_OK)
            {
                lv_label_set_text_fmt(s_wifi_status, "WIFI SCAN FAILED\n%s",
                                      esp_err_to_name(result));
                return;
            }
            lv_label_set_text(s_wifi_status, "SELECT A NETWORK TO CONNECT");
            lv_obj_clear_flag(s_wifi_results, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(s_wifi_results);
            lv_obj_scroll_to_y(s_wifi_results, 0, LV_ANIM_OFF);
            s_wifi_ssid_count = 0U;
            char *line = s_wifi_scan_text;
            while (*line != '\0' && s_wifi_ssid_count < 8U)
            {
                char *end = line;
                while (*end != '\0' && *end != '\n')
                    ++end;
                char *details = strchr(line, '\t');
                if (details == NULL || details >= end)
                    details = end;
                size_t length = (size_t)(details - line);
                while (length > 0U && (line[length - 1] == '\r' || line[length - 1] == ' '))
                    --length;
                if (details != end && length > 0U && length < sizeof(s_wifi_ssids[0]))
                {
                    memcpy(s_wifi_ssids[s_wifi_ssid_count], line, length);
                    s_wifi_ssids[s_wifi_ssid_count][length] = '\0';
                    settings_wifi_result_button(s_wifi_ssids[s_wifi_ssid_count],
                                                (lv_coord_t)s_wifi_ssid_count * 32);
                    ++s_wifi_ssid_count;
                }
                line = *end == '\n' ? end + 1 : end;
            }
            if (s_wifi_ssid_count == 0U)
                lv_label_set_text(s_wifi_status, "NO NETWORKS FOUND");
        }

        static const char *settings_ship_step_name(std::uint8_t step)
        {
            static const char *const names[] = {
                "SD CARD", "CAMERA", "TOUCH", "AUDIO", "LORA", "DISPLAY", "FINALIZE"};
            return step < kShipUiCount ? names[step] : "UNKNOWN";
        }

        extern "C" void settings_ship_progress_reset(void)
        {
            s_flow_count = kShipUiCount;
            lv_obj_clear_flag(s_flow_scroll, LV_OBJ_FLAG_HIDDEN);
            lv_obj_scroll_to_y(s_flow_scroll, 0, LV_ANIM_OFF);
            for (std::uint8_t index = 0; index < kShipUiCount; ++index)
            {
                s_ship_state[index].store(0U);
                s_ship_result[index].store(ESP_OK);
                if (s_ship_steps[index] == NULL)
                    continue;
                lv_label_set_text(s_ship_steps[index], "WAITING");
                lv_label_set_text(s_ship_names[index], settings_ship_step_name(index));
                lv_obj_set_style_text_color(s_ship_steps[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_clear_flag(s_ship_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_ship_lines[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_ship_steps[index], LV_OBJ_FLAG_HIDDEN);
            }
        }

        extern "C" void settings_sleep_progress_reset(std::uint8_t mode)
        {
            static const char *const deep_names[] = {
                "CONFIG WAKEUP", "SD CARD", "CAMERA", "TOUCH", "AUDIO", "LORA",
                "DISPLAY", "RELEASE RESOURCES", "HOLD OUTPUTS LOW", "ENTER DEEP SLEEP"};
            // Light sleep uses a confirmation-only dialog. Its progress rows are
            // intentionally hidden; the action itself is confirmed with YES.
            s_flow_count = mode == 0U ? 10U : 0U;
            if (mode == 0U)
            {
                lv_obj_clear_flag(s_flow_scroll, LV_OBJ_FLAG_HIDDEN);
                lv_obj_scroll_to_y(s_flow_scroll, 0, LV_ANIM_OFF);
            }
            for (std::uint8_t index = 0; index < kFlowUiCapacity; ++index)
            {
                s_ship_state[index].store(0U);
                s_ship_result[index].store(ESP_OK);
                if (s_ship_steps[index] == NULL)
                    continue;
                if (mode == 0U)
                {
                    lv_label_set_text(s_ship_names[index], deep_names[index]);
                    lv_label_set_text(s_ship_steps[index], "WAITING");
                    lv_obj_set_style_text_color(s_ship_steps[index],
                                                lv_color_hex(UI_WIHTE), 0);
                    lv_obj_clear_flag(s_ship_names[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_ship_lines[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_ship_steps[index], LV_OBJ_FLAG_HIDDEN);
                }
                else
                {
                    lv_obj_add_flag(s_ship_names[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(s_ship_lines[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(s_ship_steps[index], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }

        extern "C" void settings_sleep_progress_update(std::uint8_t step, bool complete,
                                                         esp_err_t result)
        {
            if (step >= s_flow_count)
                return;
            s_ship_result[step].store(result);
            s_ship_state[step].store(complete ? 2U : 1U);
        }

        extern "C" void settings_ship_progress_update(std::uint8_t step, bool complete,
                                                       esp_err_t result)
        {
            for (std::uint8_t row = 0; row < kShipUiCount; ++row)
            {
                if (kShipUiStepIds[row] == step)
                {
                    s_ship_result[row].store(result);
                    s_ship_state[row].store(complete ? 2U : 1U);
                    return;
                }
            }
        }

        static void settings_ship_ui_refresh(lv_timer_t *timer)
        {
            (void)timer;
            for (std::uint8_t step = 0; step < s_flow_count; ++step)
            {
                if (s_ship_steps[step] == NULL)
                    continue;
                const std::uint8_t state = s_ship_state[step].load();
                if (state == 0U)
                    continue;
                const esp_err_t result = static_cast<esp_err_t>(s_ship_result[step].load());
                if (state == 1U)
                {
                    lv_label_set_text(s_ship_steps[step], "RUNNING");
                    lv_obj_set_style_text_color(s_ship_steps[step], lv_color_hex(UI_WIHTE), 0);
                }
                else if (result == ESP_OK)
                {
                    lv_label_set_text(s_ship_steps[step], "COMPLETE");
                    lv_obj_set_style_text_color(s_ship_steps[step], lv_color_hex(0x7edba5), 0);
                }
                else
                {
                    lv_label_set_text_fmt(s_ship_steps[step], "FAILED: %s", esp_err_to_name(result));
                    lv_obj_set_style_text_color(s_ship_steps[step], lv_color_hex(0xe235ac), 0);
                }
            }
            if (s_ship_done.load())
            {
                s_ship_running = false;
                const esp_err_t result = static_cast<esp_err_t>(s_ship_overall_result.load());
                lv_obj_set_height(s_settings_popup_message, 40);
                if (result == ESP_ERR_INVALID_STATE &&
                    s_settings_action == SETTINGS_ACTION_LIGHT_SLEEP)
                    lv_label_set_text(s_settings_popup_message,
                                      "STOP RECORDING OR MUSIC\nBEFORE LIGHT SLEEP");
                else if (result != ESP_OK)
                    lv_label_set_text_fmt(s_settings_popup_message,
                                          s_settings_action == SETTINGS_ACTION_SHIP
                                              ? "SHIP MODE FAILED\n%s"
                                              : "SLEEP FAILED\n%s",
                                          esp_err_to_name(result));
                else if (s_settings_action == SETTINGS_ACTION_LIGHT_SLEEP)
                    lv_label_set_text(s_settings_popup_message, "LIGHT SLEEP COMPLETE");
                else if (s_settings_action == SETTINGS_ACTION_SHIP)
                    lv_label_set_text(s_settings_popup_message, "SHIP MODE ENTERED");
                else
                    lv_label_set_text(s_settings_popup_message, "SLEEP COMPLETE");
                if (s_ship_refresh_timer != NULL)
                    lv_timer_pause(s_ship_refresh_timer);
            }
        }

        static void settings_ship_worker(void *argument)
        {
            (void)argument;
            const esp_err_t result = settings_control_enter_ship_mode();
            s_ship_overall_result.store(result);
            s_ship_done.store(true);
            vTaskDelete(NULL);
        }

        static void settings_sleep_worker(void *argument)
        {
            const settings_action_t action = static_cast<settings_action_t>(
                reinterpret_cast<uintptr_t>(argument));
            const esp_err_t result = action == SETTINGS_ACTION_DEEP_SLEEP
                                         ? settings_control_enter_deep_sleep()
                                         : settings_control_enter_light_sleep();
            s_ship_overall_result.store(result);
            s_ship_done.store(true);
            vTaskDelete(NULL);
        }

        static void settings_ship_start(void)
        {
            if (s_ship_running)
                return;
            s_ship_running = true;
            s_ship_done.store(false);
            if (s_ship_refresh_timer != NULL)
                lv_timer_resume(s_ship_refresh_timer);
            if (xTaskCreate(settings_ship_worker, "ship_mode", 4096, NULL, 5, NULL) != pdPASS)
            {
                s_ship_running = false;
                lv_timer_pause(s_ship_refresh_timer);
                lv_label_set_text(s_settings_popup_message, "SHIP MODE FAILED: NO TASK MEMORY");
            }
        }

        static void settings_sleep_start(settings_action_t action)
        {
            if (s_ship_running)
                return;
            s_ship_running = true;
            s_ship_done.store(false);
            if (s_ship_refresh_timer != NULL)
                lv_timer_resume(s_ship_refresh_timer);
            if (xTaskCreate(settings_sleep_worker, "sleep_mode", 8192,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(action)), 5, NULL) != pdPASS)
            {
                s_ship_running = false;
                lv_timer_pause(s_ship_refresh_timer);
                lv_label_set_text(s_settings_popup_message, "SLEEP FAILED: NO TASK MEMORY");
            }
        }

        static void settings_wifi_keyboard_show(lv_obj_t *textarea)
        {
            if (textarea == NULL || s_wifi_keyboard == NULL)
                return;
            lv_keyboard_set_textarea(s_wifi_keyboard, textarea);
            lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_wifi_keyboard);
            lv_obj_add_state(textarea, LV_STATE_FOCUSED);
            lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
        }

        static void settings_wifi_keyboard_event_cb(lv_event_t *event)
        {
            const lv_event_code_t code = lv_event_get_code(event);
            if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL)
                return;
            if (s_wifi_keyboard != NULL)
            {
                lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
                lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
            }
            if (s_wifi_password != NULL)
                lv_obj_clear_state(s_wifi_password, LV_STATE_FOCUSED);
            if (s_wifi_ssid != NULL)
                lv_obj_clear_state(s_wifi_ssid, LV_STATE_FOCUSED);
            if (code == LV_EVENT_READY && s_wifi_status != NULL)
                lv_label_set_text(s_wifi_status, "PASSWORD READY - PRESS CONNECT");
        }

        static void settings_panel_style(lv_obj_t *panel, bool child_container = true)
        {
            stylePanel(panel);
            // Use the same blue-to-black vertical surface on every settings panel.
            static lv_grad_dsc_t gradient{};
            static bool gradient_initialized = false;
            if (!gradient_initialized)
            {
                static const lv_color_t colors[] = {
                    lv_color_hex(UI_BLUE_BG), lv_color_hex(UI_BLACK)};
                static const lv_opa_t opas[] = {LV_OPA_COVER, LV_OPA_COVER};
                static const uint8_t fractions[] = {0, 255};
                lv_grad_init_stops(&gradient, colors, opas, fractions, 2);
                lv_grad_linear_init(&gradient, 0, 0, 0, kPopupSize,
                                    LV_GRAD_EXTEND_PAD);
                gradient_initialized = true;
            }
            lv_obj_set_style_bg_grad(panel, &gradient, LV_PART_MAIN);
            lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_VER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(panel, lv_color_hex(UI_BLACK), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
            if (child_container)
            {
                lv_obj_set_style_bg_grad(panel, nullptr, LV_PART_MAIN);
                lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_NONE, LV_PART_MAIN);
                lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
                lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
                lv_obj_set_style_shadow_width(panel, 0, LV_PART_MAIN);
            }
        }

        static lv_obj_t *settings_button(lv_obj_t *parent, const char *text,
                                         lv_coord_t x, lv_coord_t y, lv_coord_t width,
                                         lv_coord_t height, lv_event_cb_t callback,
                                         void *user_data, uint32_t color)
        {
            lv_obj_t *button = settings_ui_button(parent, NULL, text, width, height,
                                              callback, user_data);
            lv_obj_set_pos(button, x, y);
            stylePanel(button, 18);
            lv_obj_set_style_bg_color(button, lv_color_hex(kActiveColor), LV_STATE_PRESSED);
            lv_obj_t *label = lv_obj_get_child(button, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
            lv_obj_set_width(label, LV_PCT(100));
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

            return button;
        }

        static void settings_check_rows_hide(void)
        {
            if (s_check_scroll != nullptr)
                lv_obj_add_flag(s_check_scroll, LV_OBJ_FLAG_HIDDEN);
            for (std::size_t index = 0; index < kCheckUiCount; ++index)
            {
                if (s_check_lines[index] != NULL)
                    lv_obj_add_flag(s_check_lines[index], LV_OBJ_FLAG_HIDDEN);
                if (s_check_names[index] != NULL)
                    lv_obj_add_flag(s_check_names[index], LV_OBJ_FLAG_HIDDEN);
                if (s_check_values[index] != NULL)
                    lv_obj_add_flag(s_check_values[index], LV_OBJ_FLAG_HIDDEN);
            }
        }

        static bool settings_check_rows_show(char *check_text)
        {
            if (check_text == NULL)
                return false;

            std::size_t index = 0U;
            char *save = NULL;
            char *line = strtok_r(check_text, "\n", &save);
            while (line != NULL && index < kCheckUiCount)
            {
                char *separator = strchr(line, '\t');
                if (separator != NULL)
                {
                    *separator = '\0';
                    const char *value = separator + 1;
                    const bool passed = strncmp(value, "OK", 2) == 0 &&
                                        (value[2] == '\0' || value[2] == ' ' || value[2] == '(');
                    lv_label_set_text(s_check_names[index], line);
                    lv_label_set_text(s_check_values[index], value);
                    lv_obj_set_style_text_color(s_check_values[index],
                                                lv_color_hex(passed ? 0x7edba5 : 0xe235ac), 0);
                    lv_obj_clear_flag(s_check_names[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_check_values[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_check_lines[index], LV_OBJ_FLAG_HIDDEN);
                    ++index;
                }
                line = strtok_r(NULL, "\n", &save);
            }
            return index == kCheckUiCount;
        }

        static void settings_popup_close_cb(lv_event_t *event)
        {
            (void)event;
            if (s_ship_running || s_wifi_scan_running.load())
                return;
            if (s_settings_popup != NULL)
                lv_obj_add_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN);
            settings_check_rows_hide();
            if (s_wifi_status != NULL)
                lv_obj_add_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
            if (s_bluetooth_status != NULL)
                lv_obj_add_flag(s_bluetooth_status, LV_OBJ_FLAG_HIDDEN);
            if (s_wifi_status_timer != NULL)
                lv_timer_pause(s_wifi_status_timer);
            if (s_bluetooth_status_timer != NULL)
                lv_timer_pause(s_bluetooth_status_timer);
            s_settings_action = SETTINGS_ACTION_NONE;
            if (s_monitor_timer != NULL)
            {
                lv_timer_del(s_monitor_timer);
                s_monitor_timer = NULL;
            }
            if (s_wifi_keyboard != NULL)
            {
                lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
                lv_keyboard_set_textarea(s_wifi_keyboard, NULL);
            }
        }

        static void settings_monitor_refresh(void)
        {
            const auto set_monitor_value = [](size_t index, const char *value) {
                if (index >= SETTINGS_POWER_ROW_COUNT || value == NULL)
                    return;

                lv_label_set_text(s_monitor_values[index], value);
                const bool negative_number = value[0] == '-';
                const bool no_value = strcmp(value, "NO") == 0;
                const uint32_t color = negative_number || no_value
                                           ? 0xe235ac
                                           : UI_WIHTE;
                lv_obj_set_style_text_color(s_monitor_values[index], lv_color_hex(color), 0);
            };

            char power_text[SETTINGS_POWER_TEXT_SIZE];
            if (settings_control_read_power(power_text, sizeof(power_text)) == ESP_OK)
            {
                char *save = NULL;
                char *line = strtok_r(power_text, "\n", &save);
                size_t index = 0U;
                while (line != NULL && index < SETTINGS_POWER_ROW_COUNT)
                {
                    char *separator = strchr(line, '\t');
                    if (separator != NULL)
                    {
                        *separator = '\0';
                        lv_label_set_text(s_monitor_names[index], line);
                        set_monitor_value(index, separator + 1);
                    }
                    line = strtok_r(NULL, "\n", &save);
                    ++index;
                }
            }
            else
            {
                // Do not leave old LED/power readings looking current.
                for (size_t index = 0; index < SETTINGS_POWER_ROW_COUNT; ++index)
                    set_monitor_value(index, "UNAVAILABLE");
            }
        }

        static void settings_charger_toggle_refresh(void)
        {
            if (s_charger_toggle == NULL)
                return;

            bool enabled = false;
            const esp_err_t result = settings_control_get_charger_enabled(&enabled);
            lv_obj_t *label = lv_obj_get_child(s_charger_toggle, 0);
            if (label == NULL)
                return;

            if (result == ESP_OK)
            {
                lv_label_set_text(label, enabled ? "CHARGER OFF" : "CHARGER ON");
                lv_obj_set_style_text_color(label, lv_color_hex(0x7edba5), 0);
            }
            else
            {
                lv_label_set_text(label, "CHARGER ERROR");
                lv_obj_set_style_text_color(label, lv_color_hex(0xe235ac), 0);
            }
        }

        static void settings_charger_toggle_cb(lv_event_t *event)
        {
            (void)event;
            bool enabled = false;
            if (settings_control_get_charger_enabled(&enabled) != ESP_OK)
            {
                settings_charger_toggle_refresh();
                return;
            }

            const esp_err_t result = settings_control_set_charger_enabled(!enabled);
            if (result != ESP_OK)
            {
                lv_obj_t *label = lv_obj_get_child(s_charger_toggle, 0);
                if (label != NULL)
                {
                    lv_label_set_text_fmt(label, "CHARGER FAILED: %s", esp_err_to_name(result));
                    lv_obj_set_style_text_color(label, lv_color_hex(0xe235ac), 0);
                }
                return;
            }
            settings_charger_toggle_refresh();
        }

        static void settings_monitor_timer_cb(lv_timer_t *timer)
        {
            (void)timer;
            if (s_settings_popup == NULL || lv_obj_has_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN))
                return;
            settings_monitor_refresh();
        }

        static void settings_sd_info_refresh(void)
        {
            for (size_t index = 0; index < 8U; ++index)
            {
                lv_label_set_text(s_sd_info_names[index], "");
                lv_label_set_text(s_sd_info_values[index], "");
            }
            char info_text[384];
            if (settings_control_read_sd_info(info_text, sizeof(info_text)) != ESP_OK)
            {
                lv_label_set_text(s_sd_info_values[0], "UNAVAILABLE");
                return;
            }
            char *save = NULL;
            char *line = strtok_r(info_text, "\n", &save);
            size_t index = 0U;
            while (line != NULL && index < 8U)
            {
                char *separator = strchr(line, '\t');
                if (separator != NULL)
                {
                    *separator = '\0';
                    lv_label_set_text(s_sd_info_names[index], line);
                    lv_label_set_text(s_sd_info_values[index], separator + 1);
                }
                line = strtok_r(NULL, "\n", &save);
                ++index;
            }
        }

        static void settings_sd_msc_toggle_cb(lv_event_t *event)
        {
            (void)event;
            const bool enabled = !settings_control_is_sd_msc_enabled();
            const esp_err_t result = settings_control_set_sd_msc_enabled(enabled);
            settings_sd_info_refresh();
            if (result != ESP_OK)
                lv_label_set_text_fmt(s_sd_info_values[0], "FAILED: %s", esp_err_to_name(result));
            lv_label_set_text(lv_obj_get_child(s_sd_msc_toggle, 0),
                              settings_control_is_sd_msc_enabled() ? "MSC ON" : "MSC OFF");
        }

        static void settings_popup_x_cb(lv_event_t *event)
        {
            settings_popup_close_cb(event);
        }

        static void settings_wifi_input_cb(lv_event_t *event)
        {
            if (s_wifi_keyboard == NULL || s_settings_popup == NULL)
                return;
            lv_obj_t *textarea = static_cast<lv_obj_t *>(lv_event_get_target(event));
            if (textarea == NULL)
                return;
            lv_obj_clear_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN);
            settings_wifi_keyboard_show(textarea);
        }

        static void settings_wifi_toggle_cb(lv_event_t *event)
        {
            (void)event;
            const esp_err_t result = wifi_control_set_enabled(!wifi_control_is_enabled());
            if (result == ESP_OK)
            {
                lv_label_set_text(s_wifi_status,
                                  wifi_control_is_enabled() ? "WIFI: ENABLED" : "WIFI: DISABLED");
                lv_label_set_text(lv_obj_get_child(s_wifi_toggle, 0),
                                  wifi_control_is_enabled() ? "WIFI OFF" : "WIFI ON");
                char status_text[96];
                if (wifi_control_get_status(status_text, sizeof(status_text)) == ESP_OK)
                    lv_label_set_text(s_wifi_status, status_text);
            }
            else
                lv_label_set_text_fmt(s_wifi_status, "WIFI TOGGLE FAILED\n%s",
                                      esp_err_to_name(result));
        }

        static void settings_bluetooth_toggle_cb(lv_event_t *event)
        {
            (void)event;
            const esp_err_t result = bluetooth_control_set_enabled(!bluetooth_control_is_enabled());
            if (result == ESP_OK)
            {
                lv_label_set_text(s_bluetooth_status,
                                  bluetooth_control_is_enabled() ? "BLE: ENABLED" : "BLE: DISABLED");
                lv_label_set_text(lv_obj_get_child(s_bluetooth_toggle, 0),
                                  bluetooth_control_is_enabled() ? "BLE OFF" : "BLE ON");
                char status_text[32];
                if (bluetooth_control_get_status(status_text, sizeof(status_text)) == ESP_OK)
                    lv_label_set_text(s_bluetooth_status, status_text);
                settings_bluetooth_info_refresh();
            }
            else
                lv_label_set_text_fmt(s_bluetooth_status, "BLE TOGGLE FAILED\n%s",
                                      esp_err_to_name(result));
        }

        static void settings_bluetooth_info_refresh(void)
        {
            bluetooth_control_info_t info{};
            if (bluetooth_control_get_info(&info) != ESP_OK)
                return;

            char rssi_text[24];
            if (info.rssi_valid)
                std::snprintf(rssi_text, sizeof(rssi_text), "%d dBm", info.rssi_dbm);
            else
                std::snprintf(rssi_text, sizeof(rssi_text), "--");

            const char *values[] = {
                info.advertised_name,
                info.local_address,
                info.connected ? "CONNECTED" : "DISCONNECTED",
                info.host_identifier,
                rssi_text,
            };
            for (size_t index = 0; index < 5U; ++index)
                lv_label_set_text(s_bluetooth_info_values[index], values[index]);
        }

        static void settings_bluetooth_status_refresh(lv_timer_t *timer)
        {
            (void)timer;
            if (s_settings_popup == NULL ||
                lv_obj_has_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN) ||
                lv_obj_has_flag(s_bluetooth_status, LV_OBJ_FLAG_HIDDEN))
                return;

            char status_text[32];
            if (bluetooth_control_get_status(status_text, sizeof(status_text)) == ESP_OK)
                lv_label_set_text(s_bluetooth_status, status_text);
            lv_label_set_text(lv_obj_get_child(s_bluetooth_toggle, 0),
                              bluetooth_control_is_enabled() ? "BLE OFF" : "BLE ON");
            settings_bluetooth_info_refresh();
        }

        static void settings_wifi_status_refresh(lv_timer_t *timer)
        {
            (void)timer;
            if (s_settings_popup == NULL ||
                lv_obj_has_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN) ||
                lv_obj_has_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN))
                return;
            if (s_wifi_scan_running.load())
            {
                lv_label_set_text(s_wifi_status, "SCANNING...");
                return;
            }
            char status_text[96];
            if (wifi_control_get_status(status_text, sizeof(status_text)) == ESP_OK)
                lv_label_set_text(s_wifi_status, status_text);
        }

        static void settings_wifi_result_cb(lv_event_t *event);

        static lv_obj_t *settings_wifi_result_button(const char *ssid, lv_coord_t y)
        {
            lv_obj_t *row = settings_ui_button(s_wifi_results, NULL, ssid, kPopupContentWidth, 30,
                                           settings_wifi_result_cb,
                                           s_wifi_ssids[s_wifi_ssid_count]);
            lv_obj_set_pos(row, 0, y);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x0d0d0d), 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(UI_BLACK), LV_STATE_PRESSED);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(row, 0),
                                        lv_color_hex(UI_WIHTE), 0);
            lv_obj_set_style_text_font(lv_obj_get_child(row, 0),
                                       &lv_font_montserrat_14, 0);
            return row;
        }

        static void settings_wifi_scan_cb(lv_event_t *event)
        {
            (void)event;
            if (s_wifi_scan_running.exchange(true))
                return;
            lv_label_set_text(s_wifi_status, "SCANNING WIFI...");
            lv_obj_add_flag(s_wifi_results, LV_OBJ_FLAG_HIDDEN);
            s_wifi_scan_done.store(false);
            s_wifi_scan_result.store(ESP_ERR_NOT_FINISHED);
            if (xTaskCreate(settings_wifi_scan_worker, "wifi_scan", 4096, NULL, 5, NULL) != pdPASS)
            {
                s_wifi_scan_running.store(false);
                lv_label_set_text(s_wifi_status, "WIFI SCAN FAILED\nNO TASK MEMORY");
            }
            else if (s_wifi_scan_timer != NULL)
            {
                lv_obj_add_state(s_wifi_toggle, LV_STATE_DISABLED);
                lv_obj_add_state(s_wifi_scan, LV_STATE_DISABLED);
                lv_obj_add_state(s_wifi_connect, LV_STATE_DISABLED);
                lv_timer_resume(s_wifi_scan_timer);
            }
        }

        static void settings_wifi_result_cb(lv_event_t *event)
        {
            const char *ssid = static_cast<const char *>(lv_event_get_user_data(event));
            if (ssid == NULL)
                return;
            lv_textarea_set_text(s_wifi_ssid, ssid);
            lv_textarea_set_text(s_wifi_password, "");
            settings_wifi_keyboard_show(s_wifi_password);
            lv_label_set_text(s_wifi_status, "ENTER PASSWORD TO CONNECT");
        }

        static void settings_wifi_connect_cb(lv_event_t *event)
        {
            (void)event;
            const esp_err_t result = wifi_control_connect(
                lv_textarea_get_text(s_wifi_ssid), lv_textarea_get_text(s_wifi_password));
            if (result == ESP_OK)
                lv_label_set_text(s_wifi_status, "WIFI CONNECTING...");
            else
                lv_label_set_text_fmt(s_wifi_status, "WIFI CONNECT FAILED\n%s",
                                      esp_err_to_name(result));
        }

        static void settings_popup_yes_cb(lv_event_t *event)
        {
            (void)event;
            switch (s_settings_action)
            {
            case SETTINGS_ACTION_SD_MSC:
            {
                const bool enabled = !settings_control_is_sd_msc_enabled();
                const esp_err_t result = settings_control_set_sd_msc_enabled(enabled);
                if (result != ESP_OK)
                {
                    lv_label_set_text_fmt(s_settings_popup_message, "SD-MSC FAILED\n%s",
                                          esp_err_to_name(result));
                    return;
                }
                break;
            }
            case SETTINGS_ACTION_DEEP_SLEEP:
                lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_settings_popup_alt, 24, kPopupActionY);
                lv_obj_set_size(s_settings_popup_alt, kPopupContentWidth, 48);
                lv_label_set_text(lv_obj_get_child(s_settings_popup_alt, 0), "CLOSE");
                lv_obj_clear_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_settings_popup_message, "DEEP SLEEP IN PROGRESS");
                settings_sleep_start(SETTINGS_ACTION_DEEP_SLEEP);
                return;
            case SETTINGS_ACTION_LIGHT_SLEEP:
                lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_settings_popup_alt, 24, kPopupActionY);
                lv_obj_set_size(s_settings_popup_alt, kPopupContentWidth, 48);
                lv_label_set_text(lv_obj_get_child(s_settings_popup_alt, 0), "CLOSE");
                lv_obj_clear_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_settings_popup_message, "LIGHT SLEEP IN PROGRESS");
                settings_sleep_start(SETTINGS_ACTION_LIGHT_SLEEP);
                return;
            case SETTINGS_ACTION_SHIP:
            {
                lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_settings_popup_alt, 24, kPopupActionY);
                lv_obj_set_size(s_settings_popup_alt, kPopupContentWidth, 48);
                lv_label_set_text(lv_obj_get_child(s_settings_popup_alt, 0), "CLOSE");
                lv_obj_clear_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s_settings_popup_message, "SHIP MODE IN PROGRESS");
                settings_ship_start();
                return;
            }
            case SETTINGS_ACTION_CHECK:
                // CHECK is handled directly by settings_system_cb.
                break;
            case SETTINGS_ACTION_NONE:
                break;
            }
            settings_popup_close_cb(NULL);
        }

        static void settings_popup_show(const char *title, const char *body,
                                        settings_action_t action, bool confirm)
        {
            if (s_settings_popup == NULL)
                return;
            lv_obj_add_flag(s_flow_scroll, LV_OBJ_FLAG_HIDDEN);
            s_settings_action = action;
            if (s_settings_menu != nullptr)
                lv_obj_add_flag(s_settings_menu, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_settings_popup);
            lv_label_set_text(s_settings_popup_title, title);
            if (s_wifi_status != NULL)
                lv_obj_add_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
            if (s_bluetooth_status != NULL)
                lv_obj_add_flag(s_bluetooth_status, LV_OBJ_FLAG_HIDDEN);
            if (s_wifi_status_timer != NULL)
                lv_timer_pause(s_wifi_status_timer);
            if (s_bluetooth_status_timer != NULL)
                lv_timer_pause(s_bluetooth_status_timer);
            settings_check_rows_hide();
            lv_label_set_text(s_settings_popup_message, body);
            lv_obj_clear_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(s_settings_popup_message, 250);
            lv_obj_set_style_text_font(s_settings_popup_message, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_settings_popup_message, lv_color_hex(UI_WIHTE), 0);
            lv_obj_add_flag(s_wifi_toggle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bluetooth_toggle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_scan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_connect, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_ssid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_password, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_results, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_close, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_monitor_scroll, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_charger_toggle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_level_slider, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_level_value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_settings_popup_x, LV_OBJ_FLAG_HIDDEN);
            for (size_t index = 0; index < SETTINGS_POWER_ROW_COUNT; ++index)
            {
                lv_obj_add_flag(s_monitor_lines[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_monitor_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_monitor_values[index], LV_OBJ_FLAG_HIDDEN);
            }
            for (size_t index = 0; index < 5U; ++index)
            {
                lv_obj_add_flag(s_bluetooth_info_lines[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_bluetooth_info_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_bluetooth_info_values[index], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_sd_msc_toggle, LV_OBJ_FLAG_HIDDEN);
            for (size_t index = 0; index < 8U; ++index)
            {
                lv_obj_add_flag(s_sd_info_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_sd_info_values[index], LV_OBJ_FLAG_HIDDEN);
            }
            for (size_t index = 0; index < kFlowUiCapacity; ++index)
            {
                lv_obj_add_flag(s_ship_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ship_steps[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ship_lines[index], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_t *yes = s_settings_popup_yes;
            lv_obj_t *no = s_settings_popup_no;
            const bool single_confirm = confirm &&
                (action == SETTINGS_ACTION_SHIP || action == SETTINGS_ACTION_DEEP_SLEEP ||
                 action == SETTINGS_ACTION_LIGHT_SLEEP);
            lv_obj_set_pos(yes, 24, kPopupActionY);
            lv_obj_set_size(yes, single_confirm ? kPopupContentWidth : 204, 48);
            lv_obj_set_pos(no, 252, kPopupActionY);
            lv_obj_set_size(no, 204, 48);
            lv_obj_set_size(s_settings_popup_alt, 204, 48);
            if (confirm)
            {
                lv_label_set_text(lv_obj_get_child(yes, 0), "YES");
                lv_label_set_text(lv_obj_get_child(no, 0), "NO");
                lv_obj_set_pos(yes, 24, kPopupActionY);
                lv_obj_set_size(yes, single_confirm ? kPopupContentWidth : 204, 48);
                lv_obj_set_pos(no, 252, kPopupActionY);
                lv_obj_set_size(no, 204, 48);
                lv_obj_clear_flag(yes, LV_OBJ_FLAG_HIDDEN);
                if (single_confirm)
                    lv_obj_add_flag(no, LV_OBJ_FLAG_HIDDEN);
                else
                    lv_obj_clear_flag(no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
            }
            else
            {
                lv_obj_clear_flag(yes, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(lv_obj_get_child(yes, 0), "OK");
                lv_label_set_text(lv_obj_get_child(no, 0), "CLOSE");
            }
        }

        static void settings_connection_open(const char *kind)
        {
            if (kind == NULL || kind[0] != 'W')
            {
                settings_popup_show("BLUETOOTH LINK", "BLE READY", SETTINGS_ACTION_NONE, false);
                char bluetooth_status_text[32];
                if (bluetooth_control_get_status(bluetooth_status_text,
                                                  sizeof(bluetooth_status_text)) == ESP_OK)
                {
                    lv_label_set_text(s_bluetooth_status, bluetooth_status_text);
                    lv_obj_clear_flag(s_bluetooth_status, LV_OBJ_FLAG_HIDDEN);
                }
                settings_bluetooth_info_refresh();
                for (size_t index = 0; index < 5U; ++index)
                {
                    lv_obj_clear_flag(s_bluetooth_info_lines[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_bluetooth_info_names[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_bluetooth_info_values[index], LV_OBJ_FLAG_HIDDEN);
                }
                lv_obj_add_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_bluetooth_toggle, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_bluetooth_toggle, 24, kPopupActionY);
                lv_obj_set_size(s_bluetooth_toggle, kPopupContentWidth, 48);
                lv_label_set_text(lv_obj_get_child(s_bluetooth_toggle, 0),
                                  bluetooth_control_is_enabled() ? "BLE OFF" : "BLE ON");
                if (s_bluetooth_status_timer != NULL)
                    lv_timer_resume(s_bluetooth_status_timer);
                return;
            }
            settings_popup_show("WIFI LINK", "WIFI READY", SETTINGS_ACTION_NONE, false);
            char wifi_status_text[96];
            if (wifi_control_get_status(wifi_status_text, sizeof(wifi_status_text)) == ESP_OK)
            {
                lv_label_set_text(s_wifi_status, wifi_status_text);
                lv_obj_clear_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
                if (s_wifi_status_timer != NULL)
                    lv_timer_resume(s_wifi_status_timer);
            }
            lv_obj_add_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_toggle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_scan, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_connect, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_ssid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_wifi_password, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_results, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_wifi_close, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_wifi_ssid, 24, 72);
            lv_obj_set_pos(s_wifi_password, 24, 128);
            lv_obj_set_pos(s_wifi_results, 24, 184);
            lv_obj_set_size(s_wifi_results, kPopupContentWidth, 112);
            lv_obj_set_pos(s_wifi_scan, 24, 306);
            lv_obj_set_pos(s_wifi_connect, 24, 360);
            lv_obj_set_pos(s_wifi_toggle, 24, kPopupActionY);
            lv_obj_set_size(s_wifi_scan, kPopupContentWidth, 48);
            lv_obj_set_size(s_wifi_connect, kPopupContentWidth, 48);
            lv_obj_set_size(s_wifi_toggle, kPopupContentWidth, 48);
            lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_wifi_toggle, 0),
                              wifi_control_is_enabled() ? "WIFI OFF" : "WIFI ON");
        }

        static void settings_monitor_cb(lv_event_t *event)
        {
            (void)event;
            char power_text[SETTINGS_POWER_TEXT_SIZE];
            const esp_err_t result = settings_control_read_power(power_text, sizeof(power_text));
            if (result == ESP_OK)
            {
                settings_popup_show("POWER MONITOR", power_text, SETTINGS_ACTION_NONE, false);
                lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_settings_popup_x, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_monitor_scroll, LV_OBJ_FLAG_HIDDEN);
                lv_obj_scroll_to_y(s_monitor_scroll, 0, LV_ANIM_OFF);
                lv_obj_clear_flag(s_charger_toggle, LV_OBJ_FLAG_HIDDEN);
                for (size_t index = 0; index < SETTINGS_POWER_ROW_COUNT; ++index)
                {
                    lv_obj_clear_flag(s_monitor_lines[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_monitor_names[index], LV_OBJ_FLAG_HIDDEN);
                    lv_obj_clear_flag(s_monitor_values[index], LV_OBJ_FLAG_HIDDEN);
                }
                settings_monitor_refresh();
                settings_charger_toggle_refresh();
                if (s_monitor_timer != NULL)
                    lv_timer_del(s_monitor_timer);
                s_monitor_timer = lv_timer_create(settings_monitor_timer_cb, 1000, NULL);
            }
            else
                settings_popup_show("POWER MONITOR", "POWER DATA UNAVAILABLE", SETTINGS_ACTION_NONE, false);
        }

        static void settings_sd_msc_cb(lv_event_t *event)
        {
            (void)event;
            settings_popup_show("SD CARD / MSC", "", SETTINGS_ACTION_NONE, false);
            lv_obj_add_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
            settings_sd_info_refresh();
            lv_obj_clear_flag(s_sd_msc_toggle, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lv_obj_get_child(s_sd_msc_toggle, 0),
                              settings_control_is_sd_msc_enabled() ? "MSC ON" : "MSC OFF");
            for (size_t index = 0; index < 8U; ++index)
            {
                lv_obj_clear_flag(s_sd_info_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(s_sd_info_values[index], LV_OBJ_FLAG_HIDDEN);
            }
        }

        enum class LevelControl { brightness, volume };
        static LevelControl s_level_control = LevelControl::brightness;

        static esp_err_t settings_level_read(std::uint8_t *percent)
        {
            return s_level_control == LevelControl::volume
                       ? settings_control_get_audio_volume(percent)
                       : settings_control_get_backlight_brightness(percent);
        }

        static void settings_level_changed_cb(lv_event_t *event)
        {
            lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
            if (slider == NULL)
                return;

            const auto percent = static_cast<std::uint8_t>(lv_slider_get_value(slider));
            const esp_err_t result = s_level_control == LevelControl::volume
                                         ? settings_control_set_audio_volume(percent)
                                         : settings_control_set_backlight_brightness(percent);
            if (result == ESP_OK)
            {
                lv_label_set_text_fmt(s_level_value, "%u%%", static_cast<unsigned>(percent));
                lv_obj_set_style_text_color(s_level_value,
                                            lv_color_hex(UI_WIHTE), 0);
                return;
            }

            std::uint8_t current{};
            if (settings_level_read(&current) == ESP_OK)
            {
                lv_slider_set_value(slider, current, LV_ANIM_OFF);
                lv_label_set_text_fmt(s_level_value, "%u%%", static_cast<unsigned>(current));
            }
            else
                lv_label_set_text(s_level_value, "ERROR");
            lv_obj_set_style_text_color(s_level_value, lv_color_hex(0xe235ac), 0);
        }

        static void settings_level_open(LevelControl control)
        {
            s_level_control = control;
            settings_popup_show(control == LevelControl::volume ? "Audio volume" : "Display brightness",
                                "", SETTINGS_ACTION_NONE, false);
            lv_obj_add_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_level_value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_level_slider, LV_OBJ_FLAG_HIDDEN);

            std::uint8_t percent{};
            const esp_err_t result = settings_level_read(&percent);
            if (result == ESP_OK)
            {
                lv_slider_set_value(s_level_slider, percent, LV_ANIM_OFF);
                lv_label_set_text_fmt(s_level_value, "%u%%", static_cast<unsigned>(percent));
                lv_obj_clear_state(s_level_slider, LV_STATE_DISABLED);
                lv_obj_set_style_text_color(s_level_value,
                                            lv_color_hex(UI_WIHTE), 0);
            }
            else
            {
                lv_label_set_text(s_level_value, "UNAVAILABLE");
                lv_obj_add_state(s_level_slider, LV_STATE_DISABLED);
                lv_obj_set_style_text_color(s_level_value, lv_color_hex(0xe235ac), 0);
            }
        }

        static void settings_system_open(settings_action_t action)
        {
            if (action == SETTINGS_ACTION_CHECK)
            {
                char check_text[768];
                const esp_err_t result = settings_control_run_check(check_text, sizeof(check_text));
                if (result != ESP_OK)
                {
                    std::snprintf(check_text, sizeof(check_text),
                                  "CHECK FAILED\n%s", esp_err_to_name(result));
                }
                settings_popup_show("About", check_text, SETTINGS_ACTION_NONE, false);
                if (result == ESP_OK && settings_check_rows_show(check_text))
                {
                    lv_label_set_text_fmt(s_settings_popup_message, "T-PANEL-P4 ROUND  |  FW %s",
                                          esp_app_get_description()->version);
                    lv_obj_set_height(s_settings_popup_message, 28);
                    lv_obj_clear_flag(s_check_scroll, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_scroll_to_y(s_check_scroll, 0, LV_ANIM_OFF);
                    lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);
                }
                else if (result != ESP_OK)
                {
                    lv_obj_set_style_text_color(s_settings_popup_message,
                                                lv_color_hex(0xe235ac), 0);
                }
                return;
            }
            const char *title = "SYSTEM ACTION";
            const char *body = "CONFIRM OPERATION";
            switch (action)
            {
            case SETTINGS_ACTION_SD_MSC:
                title = "SD-MSC";
                body = settings_control_is_sd_msc_enabled() ? "DISABLE SD-MSC MODE?" : "ENABLE SD-MSC MODE?";
                break;
            case SETTINGS_ACTION_SHIP:
                title = "SHIP MODE";
                body = "POWER DOWN FOR SHIPPING?";
                break;
            case SETTINGS_ACTION_DEEP_SLEEP:
                title = "DEEP SLEEP";
                body = "ENTER DEEP SLEEP NOW?";
                break;
            case SETTINGS_ACTION_LIGHT_SLEEP:
                title = "LIGHT SLEEP";
                body = "ENTER LIGHT SLEEP NOW?";
                break;
            case SETTINGS_ACTION_CHECK:
                title = "HARDWARE CHECK";
                body = "RUN HARDWARE CHECK?";
                break;
            case SETTINGS_ACTION_NONE:
                break;
            }
            settings_popup_show(title, body, action, true);
        }



        static void settings_system_cb(lv_event_t *event)
        {
            settings_system_open(static_cast<settings_action_t>(
                reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
        }

        static void settings_connection_cb(lv_event_t *event)
        {
            settings_connection_open(static_cast<const char *>(lv_event_get_user_data(event)));
        }

        static void settings_menu_show(bool power)
        {
            settings_popup_show(power ? "Power" : "Connection", "",
                                SETTINGS_ACTION_NONE, false);
            lv_obj_add_flag(s_settings_popup_message, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_yes, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup_no, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(s_settings_menu);
            lv_obj_clear_flag(s_settings_menu, LV_OBJ_FLAG_HIDDEN);
            if (power)
            {
                settings_button(s_settings_menu, "CHARGE / BATTERY STATUS", 0, 0, kPopupContentWidth, 52,
                                settings_monitor_cb, nullptr, UI_WIHTE);
                settings_button(s_settings_menu, "SHIP MODE", 0, 70, kPopupContentWidth, 52,
                                settings_system_cb, reinterpret_cast<void *>(SETTINGS_ACTION_SHIP), UI_WIHTE);
                settings_button(s_settings_menu, "DEEP SLEEP", 0, 140, kPopupContentWidth, 52,
                                settings_system_cb, reinterpret_cast<void *>(SETTINGS_ACTION_DEEP_SLEEP), UI_WIHTE);
                settings_button(s_settings_menu, "LIGHT SLEEP", 0, 210, kPopupContentWidth, 52,
                                settings_system_cb, reinterpret_cast<void *>(SETTINGS_ACTION_LIGHT_SLEEP), UI_WIHTE);
            }
            else
            {
                settings_button(s_settings_menu, "WIFI", 0, 0, kPopupContentWidth, 52,
                                settings_connection_cb, const_cast<char *>("WIFI"), UI_WIHTE);
                settings_button(s_settings_menu, "BLUETOOTH", 0, 70, kPopupContentWidth, 52,
                                settings_connection_cb, const_cast<char *>("BT"), UI_WIHTE);
            }
        }

        static void settings_popup_create(lv_obj_t *root)
        {
            s_settings_popup = lv_obj_create(root);
            // Keep dialogs anchored to the settings viewport.
            lv_obj_add_flag(s_settings_popup, LV_OBJ_FLAG_FLOATING);
            lv_obj_remove_flag(s_settings_popup, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_set_size(s_settings_popup, 720, 720);
            lv_obj_set_pos(s_settings_popup, 0, 0);
            lv_obj_set_style_bg_color(s_settings_popup, lv_color_hex(0x0d0d0d), 0);
            lv_obj_set_style_bg_opa(s_settings_popup, LV_OPA_70, 0);
            lv_obj_set_style_border_width(s_settings_popup, 0, 0);
            lv_obj_set_style_pad_all(s_settings_popup, 0, 0);
            lv_obj_remove_flag(s_settings_popup, LV_OBJ_FLAG_SCROLLABLE);

            s_settings_popup_body = lv_obj_create(s_settings_popup);
            lv_obj_set_size(s_settings_popup_body, kPopupSize, kPopupSize);
            lv_obj_center(s_settings_popup_body);
            settings_panel_style(s_settings_popup_body, false);


            s_settings_popup_title = page_ui_label(s_settings_popup_body, "", &lv_font_montserrat_24, UI_WIHTE);
            lv_obj_set_pos(s_settings_popup_title, 24, 22);
            lv_obj_set_width(s_settings_popup_title, kPopupContentWidth - 60);
            s_wifi_status = page_ui_label(s_settings_popup_body, "WIFI OFF",
                                          &lv_font_montserrat_14, kMutedColor);
            lv_obj_set_pos(s_wifi_status, 24, 52);
            lv_obj_set_size(s_wifi_status, kPopupContentWidth, 20);
            lv_obj_set_style_text_align(s_wifi_status, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(s_wifi_status, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_add_flag(s_wifi_status, LV_OBJ_FLAG_HIDDEN);
            s_wifi_status_timer = lv_timer_create(settings_wifi_status_refresh, 1000, NULL);
            lv_timer_pause(s_wifi_status_timer);
            s_bluetooth_status = page_ui_label(s_settings_popup_body, "BLE OFF",
                                               &lv_font_montserrat_14, kMutedColor);
            lv_obj_set_pos(s_bluetooth_status, 24, 52);
            lv_obj_set_size(s_bluetooth_status, kPopupContentWidth, 20);
            lv_obj_set_style_text_align(s_bluetooth_status, LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_long_mode(s_bluetooth_status, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_add_flag(s_bluetooth_status, LV_OBJ_FLAG_HIDDEN);
            s_bluetooth_status_timer = lv_timer_create(settings_bluetooth_status_refresh, 1000, NULL);
            lv_timer_pause(s_bluetooth_status_timer);
            static const char *const bluetooth_info_names[] = {
                "ADVERTISED NAME", "LOCAL BD ADDRESS", "CONNECTION STATUS",
                "HOST IDENTIFIER", "RSSI"};
            static lv_point_precise_t bluetooth_separator_points[] = {{0, 0}, {kPopupContentWidth, 0}};
            for (size_t index = 0; index < 5U; ++index)
            {
                const lv_coord_t row_y = 74 + static_cast<lv_coord_t>(index) * 40;
                s_bluetooth_info_names[index] = lv_label_create(s_settings_popup_body);
                lv_obj_set_pos(s_bluetooth_info_names[index], 24, row_y);
                lv_obj_set_size(s_bluetooth_info_names[index], 200, 32);
                lv_obj_set_style_text_font(s_bluetooth_info_names[index],
                                           &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(s_bluetooth_info_names[index],
                                            lv_color_hex(UI_WIHTE), 0);
                lv_label_set_text(s_bluetooth_info_names[index], bluetooth_info_names[index]);
                s_bluetooth_info_values[index] = lv_label_create(s_settings_popup_body);
                lv_obj_set_pos(s_bluetooth_info_values[index], 240, row_y);
                lv_obj_set_size(s_bluetooth_info_values[index], 216, 32);
                lv_obj_set_style_text_font(s_bluetooth_info_values[index],
                                           &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(s_bluetooth_info_values[index],
                                            lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_bluetooth_info_values[index],
                                            LV_TEXT_ALIGN_RIGHT, 0);
                lv_label_set_long_mode(s_bluetooth_info_values[index], LV_LABEL_LONG_MODE_DOTS);
                s_bluetooth_info_lines[index] = lv_line_create(s_settings_popup_body);
                lv_obj_set_pos(s_bluetooth_info_lines[index], 24, row_y + 34);
                lv_obj_set_size(s_bluetooth_info_lines[index], kPopupContentWidth, 1);
                lv_line_set_points(s_bluetooth_info_lines[index], bluetooth_separator_points, 2);
                lv_obj_set_style_line_width(s_bluetooth_info_lines[index], 1, 0);
                lv_obj_set_style_line_color(s_bluetooth_info_lines[index],
                                            lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_line_opa(s_bluetooth_info_lines[index], LV_OPA_30, 0);
                lv_obj_add_flag(s_bluetooth_info_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_bluetooth_info_values[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_bluetooth_info_lines[index], LV_OBJ_FLAG_HIDDEN);
            }
            s_settings_popup_message = page_ui_label(s_settings_popup_body, "", &lv_font_montserrat_14, UI_WIHTE);
            lv_obj_set_pos(s_settings_popup_message, 24, 72);
            lv_obj_set_width(s_settings_popup_message, kPopupContentWidth);
            lv_obj_set_height(s_settings_popup_message, 250);
            lv_label_set_long_mode(s_settings_popup_message, LV_LABEL_LONG_MODE_WRAP);
            // Center the circular close button on the title's 24px text line.
            s_settings_popup_x = settings_button(s_settings_popup_body, LV_SYMBOL_CLOSE, 412, 14, 44, 44,
                                                 settings_popup_x_cb, NULL, UI_WIHTE);
            lv_obj_set_style_border_width(s_settings_popup_x, 0, 0);
            lv_obj_set_style_shadow_width(s_settings_popup_x, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(s_settings_popup_x, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_add_flag(s_settings_popup_x, LV_OBJ_FLAG_HIDDEN);
            lv_obj_t *close_label = lv_obj_get_child(s_settings_popup_x, 0);
            if (close_label != NULL)
            {
                lv_obj_set_size(close_label, LV_PCT(100), LV_SIZE_CONTENT);
                lv_obj_align(close_label, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_text_align(close_label, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_set_style_text_font(close_label, &lv_font_montserrat_16, 0);
            }
            s_level_value = page_ui_label(s_settings_popup_body, "50%",
                                               &lv_font_montserrat_28, UI_WIHTE);
            lv_obj_set_pos(s_level_value, 24, 180);
            lv_obj_set_width(s_level_value, kPopupContentWidth);
            lv_obj_set_style_text_align(s_level_value, LV_TEXT_ALIGN_CENTER, 0);
            s_level_slider = lv_slider_create(s_settings_popup_body);
            lv_obj_set_size(s_level_slider, kPopupContentWidth - 32, 20);
            lv_obj_set_pos(s_level_slider, 40, 246);
            lv_slider_set_range(s_level_slider, 0, 100);
            lv_obj_set_style_bg_color(s_level_slider, lv_color_hex(UI_GTAY), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_level_slider, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(s_level_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_level_slider, lv_color_hex(UI_WIHTE),
                                      LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(s_level_slider, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(s_level_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_level_slider, lv_color_hex(UI_WIHTE),
                                      LV_PART_KNOB);
            lv_obj_set_style_radius(s_level_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_add_event_cb(s_level_slider, settings_level_changed_cb,
                                LV_EVENT_VALUE_CHANGED, NULL);
            lv_obj_add_flag(s_level_value, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_level_slider, LV_OBJ_FLAG_HIDDEN);
            s_monitor_scroll = lv_obj_create(s_settings_popup_body);
            lv_obj_set_size(s_monitor_scroll, kPopupContentWidth, 324);
            lv_obj_set_pos(s_monitor_scroll, 24, 72);
            settings_panel_style(s_monitor_scroll);
            lv_obj_add_flag(s_monitor_scroll, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_monitor_scroll, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_set_scroll_dir(s_monitor_scroll, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(s_monitor_scroll, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_style_pad_bottom(s_monitor_scroll, 8, 0);
            lv_obj_add_flag(s_monitor_scroll, LV_OBJ_FLAG_HIDDEN);
            static lv_point_precise_t separator_points[] = {{0, 0}, {kPopupContentWidth, 0}};
            for (size_t index = 0; index < SETTINGS_POWER_ROW_COUNT; ++index)
            {
                const lv_coord_t row_y = (lv_coord_t)index * 40;
                s_monitor_names[index] = lv_label_create(s_monitor_scroll);
                lv_obj_set_pos(s_monitor_names[index], 0, row_y);
                lv_obj_set_size(s_monitor_names[index], 200, 32);
                lv_obj_set_style_text_font(s_monitor_names[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_monitor_names[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_monitor_names[index], LV_TEXT_ALIGN_LEFT, 0);
                s_monitor_values[index] = lv_label_create(s_monitor_scroll);
                lv_obj_set_pos(s_monitor_values[index], 216, row_y);
                lv_obj_set_size(s_monitor_values[index], 216, 32);
                lv_obj_set_style_text_font(s_monitor_values[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_monitor_values[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_monitor_values[index], LV_TEXT_ALIGN_RIGHT, 0);
                s_monitor_lines[index] = lv_line_create(s_monitor_scroll);
                lv_obj_set_pos(s_monitor_lines[index], 0, row_y + 34);
                lv_obj_set_size(s_monitor_lines[index], kPopupContentWidth, 1);
                lv_line_set_points(s_monitor_lines[index], separator_points, 2);
                lv_obj_set_style_line_width(s_monitor_lines[index], 1, 0);
                lv_obj_set_style_line_color(s_monitor_lines[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_line_opa(s_monitor_lines[index], LV_OPA_30, 0);
                lv_obj_add_flag(s_monitor_lines[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_monitor_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_monitor_values[index], LV_OBJ_FLAG_HIDDEN);
            }
            s_charger_toggle = settings_button(s_settings_popup_body, "CHARGER OFF", 24, kPopupActionY, kPopupContentWidth, 48,
                                               settings_charger_toggle_cb, NULL, 0x7edba5);
            lv_obj_add_flag(s_charger_toggle, LV_OBJ_FLAG_HIDDEN);
            s_check_scroll = lv_obj_create(s_settings_popup_body);
            lv_obj_set_pos(s_check_scroll, 24, 112);
            lv_obj_set_size(s_check_scroll, kPopupContentWidth, 288);
            settings_panel_style(s_check_scroll);
            lv_obj_add_flag(s_check_scroll, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_check_scroll, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_set_scroll_dir(s_check_scroll, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(s_check_scroll, LV_SCROLLBAR_MODE_OFF);
            lv_obj_add_flag(s_check_scroll, LV_OBJ_FLAG_HIDDEN);
            static lv_point_precise_t check_separator_points[] = {{0, 0}, {kPopupContentWidth, 0}};
            for (std::size_t index = 0; index < kCheckUiCount; ++index)
            {
                const lv_coord_t row_y = static_cast<lv_coord_t>(index) * 40;
                s_check_names[index] = lv_label_create(s_check_scroll);
                lv_obj_set_pos(s_check_names[index], 0, row_y);
                lv_obj_set_size(s_check_names[index], 128, 32);
                lv_obj_set_style_text_font(s_check_names[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_check_names[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_check_names[index], LV_TEXT_ALIGN_LEFT, 0);
                s_check_values[index] = lv_label_create(s_check_scroll);
                lv_obj_set_pos(s_check_values[index], 144, row_y);
                lv_obj_set_size(s_check_values[index], kPopupContentWidth - 144, 32);
                lv_obj_set_style_text_font(s_check_values[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_check_values[index], lv_color_hex(0x7edba5), 0);
                lv_obj_set_style_text_align(s_check_values[index], LV_TEXT_ALIGN_RIGHT, 0);
                lv_label_set_long_mode(s_check_values[index], LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
                s_check_lines[index] = lv_line_create(s_check_scroll);
                lv_obj_set_pos(s_check_lines[index], 0, row_y + 34);
                lv_obj_set_size(s_check_lines[index], kPopupContentWidth, 1);
                lv_line_set_points(s_check_lines[index], check_separator_points, 2);
                lv_obj_set_style_line_width(s_check_lines[index], 1, 0);
                lv_obj_set_style_line_color(s_check_lines[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_line_opa(s_check_lines[index], LV_OPA_30, 0);
                lv_obj_add_flag(s_check_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_check_values[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_check_lines[index], LV_OBJ_FLAG_HIDDEN);
            }
            for (size_t index = 0; index < 8U; ++index)
            {
                const lv_coord_t row_y = 74 + (lv_coord_t)index * 40;
                s_sd_info_names[index] = lv_label_create(s_settings_popup_body);
                lv_obj_set_pos(s_sd_info_names[index], 24, row_y);
                lv_obj_set_size(s_sd_info_names[index], 184, 32);
                lv_obj_set_style_text_font(s_sd_info_names[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_sd_info_names[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_sd_info_names[index], LV_TEXT_ALIGN_LEFT, 0);
                s_sd_info_values[index] = lv_label_create(s_settings_popup_body);
                lv_obj_set_pos(s_sd_info_values[index], 216, row_y);
                lv_obj_set_size(s_sd_info_values[index], 240, 32);
                lv_obj_set_style_text_font(s_sd_info_values[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_sd_info_values[index], lv_color_hex(0x7edba5), 0);
                lv_obj_set_style_text_align(s_sd_info_values[index], LV_TEXT_ALIGN_RIGHT, 0);
                lv_obj_add_flag(s_sd_info_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_sd_info_values[index], LV_OBJ_FLAG_HIDDEN);
            }
            s_flow_scroll = lv_obj_create(s_settings_popup_body);
            lv_obj_set_pos(s_flow_scroll, 24, 112);
            lv_obj_set_size(s_flow_scroll, kPopupContentWidth, 288);
            settings_panel_style(s_flow_scroll);
            lv_obj_add_flag(s_flow_scroll, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_flow_scroll, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_set_scroll_dir(s_flow_scroll, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(s_flow_scroll, LV_SCROLLBAR_MODE_OFF);
            lv_obj_add_flag(s_flow_scroll, LV_OBJ_FLAG_HIDDEN);
            static lv_point_precise_t ship_separator_points[] = {{0, 0}, {kPopupContentWidth, 0}};
            for (std::uint8_t index = 0; index < kFlowUiCapacity; ++index)
            {
                const lv_coord_t row_y = (lv_coord_t)index * 36;
                s_ship_names[index] = lv_label_create(s_flow_scroll);
                lv_obj_set_pos(s_ship_names[index], 0, row_y);
                lv_obj_set_size(s_ship_names[index], 200, 30);
                lv_obj_set_style_text_font(s_ship_names[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_ship_names[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_ship_names[index], LV_TEXT_ALIGN_LEFT, 0);
                lv_label_set_text(s_ship_names[index], settings_ship_step_name(index));
                s_ship_steps[index] = lv_label_create(s_flow_scroll);
                lv_obj_set_pos(s_ship_steps[index], 216, row_y);
                lv_obj_set_size(s_ship_steps[index], 216, 30);
                lv_obj_set_style_text_font(s_ship_steps[index], &lv_font_montserrat_16, 0);
                lv_obj_set_style_text_color(s_ship_steps[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_text_align(s_ship_steps[index], LV_TEXT_ALIGN_RIGHT, 0);
                s_ship_lines[index] = lv_line_create(s_flow_scroll);
                lv_obj_set_pos(s_ship_lines[index], 0, row_y + 32);
                lv_obj_set_size(s_ship_lines[index], kPopupContentWidth, 1);
                lv_line_set_points(s_ship_lines[index], ship_separator_points, 2);
                lv_obj_set_style_line_width(s_ship_lines[index], 1, 0);
                lv_obj_set_style_line_color(s_ship_lines[index], lv_color_hex(UI_WIHTE), 0);
                lv_obj_set_style_line_opa(s_ship_lines[index], LV_OPA_30, 0);
                lv_obj_add_flag(s_ship_names[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ship_steps[index], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ship_lines[index], LV_OBJ_FLAG_HIDDEN);
            }
            s_sd_msc_toggle = settings_button(s_settings_popup_body, "MSC OFF", 24, 410, kPopupContentWidth, 48,
                                              settings_sd_msc_toggle_cb, NULL, UI_WIHTE);
            lv_obj_add_flag(s_sd_msc_toggle, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_sd_msc_toggle, LV_ALIGN_BOTTOM_MID, 0, -16);

            s_settings_popup_yes = settings_button(s_settings_popup_body, "YES", 24, 350, 204, 48,
                                                   settings_popup_yes_cb, NULL, 0x7edba5);
            s_settings_popup_no = settings_button(s_settings_popup_body, "NO", 252, 350, 204, 48,
                                                  settings_popup_close_cb, NULL, 0xe235ac);
            s_settings_popup_alt = settings_button(s_settings_popup_body, "CLOSE", 252, 350, 204, 48,
                                                   settings_popup_close_cb, NULL, 0xe235ac);
            lv_obj_add_flag(s_settings_popup_alt, LV_OBJ_FLAG_HIDDEN);

            s_wifi_toggle = settings_button(s_settings_popup_body, "WIFI ON", 24, kPopupActionY, kPopupContentWidth, 48,
                                            settings_wifi_toggle_cb, NULL, UI_WIHTE);
            s_bluetooth_toggle = settings_button(s_settings_popup_body, "BLE ON", 24, kPopupActionY, kPopupContentWidth, 48,
                                                 settings_bluetooth_toggle_cb, NULL, 0xe235ac);
            lv_obj_add_flag(s_bluetooth_toggle, LV_OBJ_FLAG_HIDDEN);
            s_wifi_scan = settings_button(s_settings_popup_body, "SCAN", 24, 306, kPopupContentWidth, 48,
                                          settings_wifi_scan_cb, NULL, UI_WIHTE);
            s_wifi_connect = settings_button(s_settings_popup_body, "CONNECT", 24, 360, kPopupContentWidth, 48,
                                             settings_wifi_connect_cb, NULL, UI_WIHTE);
            s_wifi_close = settings_button(s_settings_popup_body, "CLOSE", 288, kPopupActionY, 168, 48,
                                           settings_popup_close_cb, NULL, 0xe235ac);
            lv_obj_add_flag(s_wifi_close, LV_OBJ_FLAG_HIDDEN);
            s_wifi_ssid = lv_textarea_create(s_settings_popup_body);
            lv_obj_set_size(s_wifi_ssid, kPopupContentWidth, 48);
            lv_obj_set_pos(s_wifi_ssid, 24, 72);
            lv_textarea_set_placeholder_text(s_wifi_ssid, "SSID");
            lv_textarea_set_one_line(s_wifi_ssid, true);
            lv_textarea_set_max_length(s_wifi_ssid, 32);
            lv_obj_set_style_text_font(s_wifi_ssid, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_wifi_ssid, lv_color_hex(UI_WIHTE), 0);
            lv_obj_set_style_border_width(s_wifi_ssid, 0, 0);
            lv_obj_set_style_shadow_width(s_wifi_ssid, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_wifi_ssid, lv_color_hex(UI_BLACK), 0);
            lv_obj_set_style_bg_opa(s_wifi_ssid, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(s_wifi_ssid, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_HIDDEN |
                                                                      LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(s_wifi_ssid, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_wifi_ssid, settings_wifi_input_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(s_wifi_ssid, settings_wifi_input_cb, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(s_wifi_ssid, settings_wifi_input_cb, LV_EVENT_RELEASED, NULL);
            lv_obj_add_event_cb(s_wifi_ssid, settings_wifi_input_cb, LV_EVENT_FOCUSED, NULL);
            s_wifi_password = lv_textarea_create(s_settings_popup_body);
            lv_obj_set_size(s_wifi_password, kPopupContentWidth, 48);
            lv_obj_set_pos(s_wifi_password, 24, 128);
            lv_textarea_set_placeholder_text(s_wifi_password, "PASSWORD");
            lv_textarea_set_one_line(s_wifi_password, true);
            lv_textarea_set_max_length(s_wifi_password, 64);
            lv_textarea_set_password_mode(s_wifi_password, true);
            lv_obj_set_style_text_font(s_wifi_password, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(s_wifi_password, lv_color_hex(UI_WIHTE), 0);
            lv_obj_set_style_border_width(s_wifi_password, 0, 0);
            lv_obj_set_style_shadow_width(s_wifi_password, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_wifi_password, lv_color_hex(UI_BLACK), 0);
            lv_obj_set_style_bg_opa(s_wifi_password, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(s_wifi_password, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_HIDDEN |
                                                                          LV_OBJ_FLAG_SCROLLABLE));
            lv_obj_add_flag(s_wifi_password, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_wifi_password, settings_wifi_input_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(s_wifi_password, settings_wifi_input_cb, LV_EVENT_PRESSED, NULL);
            lv_obj_add_event_cb(s_wifi_password, settings_wifi_input_cb, LV_EVENT_RELEASED, NULL);
            lv_obj_add_event_cb(s_wifi_password, settings_wifi_input_cb, LV_EVENT_FOCUSED, NULL);
            s_wifi_results = lv_obj_create(s_settings_popup_body);
            s_wifi_scan_timer = lv_timer_create(settings_wifi_scan_refresh, 100, NULL);
            lv_timer_pause(s_wifi_scan_timer);
            /* Keep the result viewport above the fixed credential fields.  The
             * viewport scrolls internally when more than four rows are returned. */
            lv_obj_set_size(s_wifi_results, kPopupContentWidth, 112);
            lv_obj_set_pos(s_wifi_results, 24, 184);
            lv_obj_set_style_bg_color(s_wifi_results, lv_color_hex(UI_BLACK), 0);
            lv_obj_set_style_bg_opa(s_wifi_results, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s_wifi_results, 0, 0);
            lv_obj_set_style_shadow_width(s_wifi_results, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_wifi_results, 0, 0);
            lv_obj_set_style_radius(s_wifi_results, 0, 0);
            lv_obj_add_flag(s_wifi_results, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_scroll_dir(s_wifi_results, LV_DIR_VER);
            lv_obj_set_scrollbar_mode(s_wifi_results, LV_SCROLLBAR_MODE_OFF);
            lv_obj_add_flag(s_wifi_results, LV_OBJ_FLAG_HIDDEN);
            s_wifi_keyboard = lv_keyboard_create(root);
            lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_FLOATING);
            lv_obj_remove_flag(s_wifi_keyboard, LV_OBJ_FLAG_SCROLL_CHAIN);
            const lv_style_selector_t keyboard_pressed = static_cast<lv_style_selector_t>(
                static_cast<uint32_t>(LV_PART_ITEMS) |
                static_cast<uint32_t>(LV_STATE_PRESSED));
            lv_obj_set_size(s_wifi_keyboard, 560, 220);
            // Keep every key within the circular display, below the credential fields.
            lv_obj_align(s_wifi_keyboard, LV_ALIGN_CENTER, 0, 90);
            lv_obj_set_style_bg_color(s_wifi_keyboard, lv_color_hex(0x0d0d0d), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_wifi_keyboard, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_wifi_keyboard, lv_color_hex(UI_BLACK), LV_PART_ITEMS);
            lv_obj_set_style_bg_opa(s_wifi_keyboard, LV_OPA_40,
                                    keyboard_pressed);
            lv_obj_set_style_text_color(s_wifi_keyboard, lv_color_hex(UI_WIHTE), LV_PART_ITEMS);
            lv_obj_set_style_text_font(s_wifi_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
            lv_obj_set_style_border_width(s_wifi_keyboard, 0, LV_PART_ITEMS);
            lv_obj_set_style_radius(s_wifi_keyboard, 0, LV_PART_ITEMS);
            lv_obj_add_event_cb(s_wifi_keyboard, settings_wifi_keyboard_event_cb, LV_EVENT_READY, NULL);
            lv_obj_add_event_cb(s_wifi_keyboard, settings_wifi_keyboard_event_cb, LV_EVENT_CANCEL, NULL);
            lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN);
            s_settings_menu = lv_obj_create(s_settings_popup_body);
            lv_obj_set_pos(s_settings_menu, 24, 112);
            lv_obj_set_size(s_settings_menu, kPopupContentWidth, 288);
            settings_panel_style(s_settings_menu);
            lv_obj_add_flag(s_settings_menu, LV_OBJ_FLAG_HIDDEN);
            s_ship_refresh_timer = lv_timer_create(settings_ship_ui_refresh, 100, NULL);
            lv_timer_pause(s_ship_refresh_timer);
        }


        void cardClicked(lv_event_t *event)
        {
            const auto index = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(
                lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
            switch (index)
            {
            case 0: settings_level_open(LevelControl::brightness); break;
            case 1: settings_level_open(LevelControl::volume); break;
            case 2: settings_sd_msc_cb(nullptr); break;
            case 3: settings_menu_show(false); break;
            case 4: settings_system_open(SETTINGS_ACTION_CHECK); break;
            case 5: settings_menu_show(true); break;
            default: break;
            }
        }

        lv_obj_t *createCard(lv_obj_t *parent, std::size_t index,
                             const char *icon, const char *title,
                             const char *description)
        {
            lv_obj_t *card = lv_obj_create(parent);
            if (card == nullptr)
                return nullptr;
            lv_obj_set_size(card, kCardWidth, kCardHeight);
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(card, reinterpret_cast<void *>(static_cast<std::uintptr_t>(index)));
            lv_obj_add_event_cb(card, cardClicked, LV_EVENT_CLICKED, nullptr);
            stylePanel(card, 20);
            lv_obj_set_style_bg_color(card, lv_color_hex(kActiveColor), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(card, LV_OPA_30, LV_STATE_PRESSED);

            lv_obj_t *icon_label = setLabel(card, icon, &lv_font_montserrat_24, UI_WIHTE);
            if (icon_label != nullptr)
                lv_obj_set_pos(icon_label, 18, 16);
            lv_obj_t *title_label = setLabel(card, title,
                                             &lv_font_source_han_sans_sc_16_cjk, UI_WIHTE);
            if (title_label != nullptr)
                lv_obj_set_pos(title_label, 62, 16);
            lv_obj_t *description_label = setLabel(card, description,
                                                   &lv_font_montserrat_14, kMutedColor);
            if (description_label != nullptr)
            {
                lv_obj_set_width(description_label, kCardWidth - 78);
                lv_obj_set_pos(description_label, 62, 46);
            }
            return card;
        }

        void setPageCreate(Page *page)
        {
            if (page == nullptr || page->root == nullptr)
                return;
            s_set = SetUi{};
            s_set.root = page->root;
            ui_create_background(page->root);
            lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);
            lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *heading = setLabel(page->root, "Settings",
                                         &lv_font_montserrat_24, UI_WIHTE);
            lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 120, 100);
            lv_obj_t *subtitle = setLabel(page->root, "Device configuration",
                                           &lv_font_montserrat_14, kMutedColor);
            lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 122, 128);

            lv_obj_t *grid = page_ui_container(page->root, kPanelWidth, 312);
            if (grid == nullptr)
                return;
            lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 192);
            lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);

            struct CardInfo
            {
                const char *icon;
                const char *title;
                const char *description;
            };
            constexpr CardInfo card_info[] = {
                {LV_SYMBOL_IMAGE, "Display", "Brightness and screen"},
                {LV_SYMBOL_VOLUME_MAX, "Audio", "Output volume"},
                {LV_SYMBOL_SD_CARD, "Storage", "SD card and files"},
                {LV_SYMBOL_WIFI, "Connection", "Wi-Fi and Bluetooth"},
                {LV_SYMBOL_BULLET, "About", "Hardware check results"},
                {LV_SYMBOL_POWER, "Power", "Battery and sleep"},
            };

            for (std::size_t index = 0; index < 6; ++index)
            {
                s_set.cards[index] = createCard(grid, index,
                                                card_info[index].icon,
                                                card_info[index].title,
                                                card_info[index].description);
                if (s_set.cards[index] == nullptr)
                    continue;
                const lv_coord_t column = static_cast<lv_coord_t>(index % 2U);
                const lv_coord_t row = static_cast<lv_coord_t>(index / 2U);
                lv_obj_set_pos(s_set.cards[index],
                               column * (kCardWidth + kColumnGap),
                               row * (kCardHeight + kRowGap));
            }

            settings_popup_create(page->root);
        }

        void setPageDestroy(Page *)
        {
            // LVGL timers are independent of the objects they update.
            lv_timer_t **timers[] = {&s_ship_refresh_timer, &s_monitor_timer,
                                    &s_wifi_status_timer, &s_bluetooth_status_timer,
                                    &s_wifi_scan_timer};
            for (lv_timer_t **timer : timers)
            {
                if (*timer != nullptr)
                    lv_timer_del(*timer);
                *timer = nullptr;
            }
            for (std::size_t i = 0; i < kFlowUiCapacity; ++i)
            {
                s_ship_names[i] = nullptr;
                s_ship_steps[i] = nullptr;
                s_ship_lines[i] = nullptr;
            }
            s_settings_popup = nullptr;
            s_settings_menu = nullptr;
            s_flow_scroll = nullptr;
            s_check_scroll = nullptr;
            s_wifi_keyboard = nullptr;
            s_set = SetUi{};
        }

        void setPageGesture(Page *page, GestureDirection direction)
        {
            if (s_ship_running || s_wifi_scan_running.load())
                return;
            if (s_settings_popup != nullptr &&
                !lv_obj_has_flag(s_settings_popup, LV_OBJ_FLAG_HIDDEN))
            {
                if (direction == GESTURE_RIGHT)
                    settings_popup_close_cb(nullptr);
                return;
            }
            page_ui_back_gesture(page, direction);
        }

        void setPageLeave(Page *)
        {
            (void)ui_page_destroy(PAGE_SET);
        }
    } // namespace

    void registerSettingsPage()
    {
        Page page{};
        page.id = PAGE_SET;
        page.name = "settings";
        page.on_create = setPageCreate;
        page.on_destroy = setPageDestroy;
        page.on_leave = setPageLeave;
        page.on_gesture = setPageGesture;
        (void)ui_page_register(&page);
    }
} // namespace factory::ui

extern "C" void settings_page_register(void)
{
    factory::ui::registerSettingsPage();
}
