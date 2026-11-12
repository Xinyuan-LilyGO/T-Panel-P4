#include "page_common.hpp"
#include "page_registry.hpp"
#include <services/lora_control.h>
#include <services/service_context.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <algorithm>

#include <esp_timer.h>

namespace factory::ui
{
    namespace
    {
        // Shared column and row spacing keep controls inside the 720px round display.
        constexpr lv_coord_t kContentWidth = 480;
        constexpr lv_coord_t kContentLeft = (720 - kContentWidth) / 2;
        constexpr lv_coord_t kRowGap = 16;
        constexpr lv_coord_t kStatusTop = 176;
        constexpr lv_coord_t kStatusHeight = 72;
        constexpr lv_coord_t kMessageTop = kStatusTop + kStatusHeight + kRowGap;
        constexpr lv_coord_t kMessageHeight = 240;
        constexpr lv_coord_t kInputTop = kMessageTop + kMessageHeight + kRowGap;
        constexpr lv_coord_t kInputHeight = 48;
        constexpr lv_coord_t kKeyboardInputTop = 276;
        constexpr lv_coord_t kSendWidth = 100;
        constexpr lv_coord_t kInputWidth = kContentWidth - kRowGap - kSendWidth;
        constexpr lv_coord_t kModeTop = kInputTop + kInputHeight + kRowGap;
        constexpr lv_coord_t kModeButtonWidth = 150;
        constexpr lv_coord_t kModeButtonHeight = 44;
        constexpr std::size_t kMaxMessages = 20;
        constexpr lv_coord_t kMessageBubbleHeight = 58;
        constexpr lv_coord_t kMessageBubbleGap = 8;
        constexpr lv_coord_t kMessageBubbleWidth = 330;
        constexpr std::size_t kSettingCount = 6;

        enum class LoraMode : std::uint8_t
        {
            manual,
            timed,
            receive,
        };

        struct LoraUi
        {
            lv_obj_t *root{};
            lv_obj_t *status{};
            lv_obj_t *frequency{};
            lv_obj_t *messages{};
            lv_obj_t *empty_message{};
            lv_obj_t *message_hint{};
            lv_obj_t *input{};
            lv_obj_t *send{};
            lv_obj_t *keyboard{};
            lv_obj_t *timed{};
            lv_obj_t *receive{};
            lv_obj_t *settings{};
            lv_obj_t *settings_panel{};
            lv_obj_t *settings_dropdowns[kSettingCount]{};
            lv_timer_t *event_timer{};
            lv_obj_t *message_items[kMaxMessages]{};
            bool message_is_tx[kMaxMessages]{};
            std::size_t message_count{};
            uint32_t saved_settings[kSettingCount]{1, 0, 0, 0, 10, 0};
            LoraMode mode{LoraMode::manual};
        };

        LoraUi s_lora;

        lv_obj_t *loraLabel(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
        {
            return page_ui_label(parent, text, font, color);
        }

        void stylePanel(lv_obj_t *object, lv_coord_t radius = 24)
        {
            if (object == nullptr)
                return;
            page_ui_apply_icon_style(object);
            lv_obj_set_style_radius(object, radius, LV_PART_MAIN);
            lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
            lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
        }

        lv_obj_t *makeButton(lv_obj_t *parent, const char *text,
                             lv_coord_t width, lv_coord_t height,
                             lv_event_cb_t callback, void *user_data)
        {
            lv_obj_t *button = lv_obj_create(parent);
            if (button == nullptr)
                return nullptr;
            lv_obj_set_size(button, width, height);
            lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
            page_ui_apply_icon_style(button);
            lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_STATE_PRESSED);
            lv_obj_set_style_text_color(button, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
            if (callback != nullptr)
                lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

            lv_obj_t *label = loraLabel(button, text, &lv_font_montserrat_16,
                                        UI_WIHTE);
            if (label != nullptr)
                lv_obj_center(label);
            return button;
        }

        void setStatus(const char *text)
        {
            if (s_lora.status != nullptr && text != nullptr)
                lv_label_set_text(s_lora.status, text);
        }

        void formatMessageTime(char *buffer, std::size_t size)
        {
            const std::uint64_t seconds = static_cast<std::uint64_t>(
                esp_timer_get_time() / 1000000LL);
            std::snprintf(buffer, size, "%02llu:%02llu:%02llu",
                          static_cast<unsigned long long>((seconds / 3600U) % 100U),
                          static_cast<unsigned long long>((seconds / 60U) % 60U),
                          static_cast<unsigned long long>(seconds % 60U));
        }

        void layoutMessages()
        {
            if (s_lora.messages == nullptr)
                return;
            for (std::size_t index = 0; index < s_lora.message_count; ++index)
            {
                lv_obj_t *item = s_lora.message_items[index];
                if (item == nullptr)
                    continue;
                lv_obj_set_pos(item,
                               s_lora.message_is_tx[index] ?
                                   kContentWidth - kMessageBubbleWidth - 12 : 12,
                               12 + static_cast<lv_coord_t>(index) *
                                        (kMessageBubbleHeight + kMessageBubbleGap));
            }
            lv_obj_set_height(s_lora.messages, kMessageHeight);
            lv_obj_scroll_to_y(s_lora.messages, LV_COORD_MAX, LV_ANIM_ON);
        }

        void addMessage(bool transmitted, const char *payload, const char *time,
                        const char *metric)
        {
            if (s_lora.messages == nullptr || payload == nullptr)
                return;
            if (s_lora.message_count == kMaxMessages)
            {
                if (s_lora.message_items[0] != nullptr)
                    lv_obj_delete(s_lora.message_items[0]);
                for (std::size_t index = 1; index < kMaxMessages; ++index)
                {
                    s_lora.message_items[index - 1] = s_lora.message_items[index];
                    s_lora.message_is_tx[index - 1] = s_lora.message_is_tx[index];
                }
                --s_lora.message_count;
            }
            lv_obj_t *bubble = lv_obj_create(s_lora.messages);
            if (bubble == nullptr)
                return;
            s_lora.message_items[s_lora.message_count++] = bubble;
            s_lora.message_is_tx[s_lora.message_count - 1] = transmitted;
            lv_obj_set_size(bubble, kMessageBubbleWidth, kMessageBubbleHeight);
            stylePanel(bubble, 16);
            lv_obj_set_style_pad_all(bubble, 8, LV_PART_MAIN);

            char header[64]{};
            std::snprintf(header, sizeof(header), "%s  %s  %s",
                          transmitted ? "TX" : "RX",
                          time != nullptr ? time : "--:--:--",
                          metric != nullptr ? metric : "");
            lv_obj_t *header_label = loraLabel(bubble, header, &lv_font_montserrat_14,
                                               transmitted ? UI_MAIN : 0x50D890);
            lv_obj_set_width(header_label, kMessageBubbleWidth - 16);
            lv_obj_set_pos(header_label, 8, 5);
            lv_obj_t *body = loraLabel(bubble, payload, &lv_font_montserrat_16,
                                       UI_WIHTE);
            lv_obj_set_width(body, kMessageBubbleWidth - 16);
            lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
            lv_obj_set_pos(body, 8, 28);
            if (s_lora.empty_message != nullptr)
                lv_obj_add_flag(s_lora.empty_message, LV_OBJ_FLAG_HIDDEN);
            if (s_lora.message_hint != nullptr)
                lv_obj_add_flag(s_lora.message_hint, LV_OBJ_FLAG_HIDDEN);
            layoutMessages();
        }

        void updateModeButtons()
        {
            const bool timed_active = s_lora.mode == LoraMode::timed;
            const bool receive_active = s_lora.mode == LoraMode::receive;
            if (s_lora.timed != nullptr)
            {
                lv_obj_set_style_bg_color(
                    s_lora.timed,
                    lv_color_hex(timed_active ? UI_MAIN : UI_BLACK),
                    LV_PART_MAIN);
                lv_obj_set_style_bg_opa(s_lora.timed, timed_active ? LV_OPA_50 : LV_OPA_10, LV_PART_MAIN);
                lv_obj_set_style_border_color(s_lora.timed, lv_color_hex(timed_active ? UI_WIHTE : UI_WIHTE), LV_PART_MAIN);
                lv_obj_set_style_border_opa(s_lora.timed, timed_active ? LV_OPA_COVER : LV_OPA_10, LV_PART_MAIN);
            }
            if (s_lora.receive != nullptr)
            {
                lv_obj_set_style_bg_color(
                    s_lora.receive,
                    lv_color_hex(receive_active ? UI_MAIN : UI_BLACK),
                    LV_PART_MAIN);
                lv_obj_set_style_bg_opa(s_lora.receive, receive_active ? LV_OPA_50 : LV_OPA_10, LV_PART_MAIN);
                lv_obj_set_style_border_color(s_lora.receive, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                lv_obj_set_style_border_opa(s_lora.receive, receive_active ? LV_OPA_COVER : LV_OPA_10, LV_PART_MAIN);
            }
        }

        void modeClicked(lv_event_t *event)
        {
            const auto mode = static_cast<LoraMode>(reinterpret_cast<std::uintptr_t>(
                lv_event_get_user_data(event)));
            if (s_lora.mode == mode)
            {
                (void)lora_control_set_idle();
                s_lora.mode = LoraMode::manual;
                updateModeButtons();
                setStatus("LoRa stopped");
                return;
            }
            const bool accepted = mode == LoraMode::timed
                ? lora_control_start_timed(1000U * (lv_dropdown_get_selected(s_lora.settings_dropdowns[5]) + 1U))
                : lora_control_start_receive();
            if (!accepted)
            {
                setStatus("LoRa command unavailable");
                return;
            }
            s_lora.mode = mode;
            updateModeButtons();
            if (mode == LoraMode::timed)
                setStatus("Timed transmit active");
            else
                setStatus("Receive active");
        }

        void keyboardHide()
        {
            if (s_lora.keyboard != nullptr)
            {
                lv_obj_add_flag(s_lora.keyboard, LV_OBJ_FLAG_HIDDEN);
                lv_keyboard_set_textarea(s_lora.keyboard, nullptr);
            }
            if (s_lora.input != nullptr)
            {
                lv_obj_clear_state(s_lora.input, LV_STATE_FOCUSED);
                lv_obj_set_y(s_lora.input, kInputTop);
            }
            if (s_lora.send != nullptr)
                lv_obj_set_y(s_lora.send, kInputTop);
        }

        void sendClicked(lv_event_t *)
        {
            if (s_lora.input == nullptr)
                return;
            const char *text = lv_textarea_get_text(s_lora.input);
            if (text == nullptr || text[0] == '\0')
            {
                setStatus("Enter a message first");
                return;
            }
            if (!lora_control_send(text))
            {
                setStatus("LoRa transmit unavailable");
                return;
            }
            lv_textarea_set_text(s_lora.input, "");
            keyboardHide();
            setStatus("Message queued for transmit");
        }

        void keyboardShow(lv_event_t *event)
        {
            if (s_lora.keyboard == nullptr)
                return;
            lv_obj_t *textarea = static_cast<lv_obj_t *>(lv_event_get_target(event));
            if (textarea == nullptr)
                return;
            // Keep the message and Send button visible above the Wi-Fi-sized keyboard.
            lv_obj_set_y(textarea, kKeyboardInputTop);
            if (s_lora.send != nullptr)
                lv_obj_set_y(s_lora.send, kKeyboardInputTop);
            lv_obj_move_foreground(textarea);
            if (s_lora.send != nullptr)
                lv_obj_move_foreground(s_lora.send);
            lv_keyboard_set_textarea(s_lora.keyboard, textarea);
            lv_obj_clear_flag(s_lora.keyboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_lora.keyboard);
            lv_obj_add_state(textarea, LV_STATE_FOCUSED);
            lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
        }

        void keyboardEvent(lv_event_t *event)
        {
            const lv_event_code_t code = lv_event_get_code(event);
            if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL)
                return;
            keyboardHide();
        }

        void updatePowerOptions();

        void settingsClicked(lv_event_t *)
        {
            if (s_lora.settings_panel == nullptr)
                return;
            keyboardHide();
            for (std::size_t index = 0; index < kSettingCount; ++index)
            {
                if (s_lora.settings_dropdowns[index] != nullptr)
                    lv_dropdown_set_selected(s_lora.settings_dropdowns[index], s_lora.saved_settings[index]);
            }
            updatePowerOptions();
            if (s_lora.saved_settings[0] >= 3U && s_lora.saved_settings[4] > 5U)
                s_lora.saved_settings[4] = 5U;
            if (s_lora.settings_dropdowns[4] != nullptr)
                lv_dropdown_set_selected(s_lora.settings_dropdowns[4], s_lora.saved_settings[4]);
            lv_obj_clear_flag(s_lora.settings_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_lora.settings_panel);
        }

        void settingsCloseClicked(lv_event_t *)
        {
            for (lv_obj_t *dropdown : s_lora.settings_dropdowns)
            {
                if (dropdown != nullptr)
                    lv_dropdown_close(dropdown);
            }
            if (s_lora.settings_panel != nullptr)
                lv_obj_add_flag(s_lora.settings_panel, LV_OBJ_FLAG_HIDDEN);
        }

        void updatePowerOptions()
        {
            lv_obj_t *power = s_lora.settings_dropdowns[4];
            lv_obj_t *frequency = s_lora.settings_dropdowns[0];
            if (power == nullptr || frequency == nullptr)
                return;
            const bool high_band = lv_dropdown_get_selected(frequency) >= 3U;
            if (high_band)
            {
                lv_dropdown_set_options_static(power, "-9 dBm\n-6 dBm\n-3 dBm\n0 dBm\n3 dBm\n5 dBm");
                if (lv_dropdown_get_selected(power) > 5U)
                    lv_dropdown_set_selected(power, 5U);
            }
            else
            {
                lv_dropdown_set_options_static(power, "-9 dBm\n-6 dBm\n-3 dBm\n0 dBm\n3 dBm\n5 dBm\n10 dBm\n14 dBm\n17 dBm\n20 dBm\n22 dBm");
            }
        }

        void frequencyChanged(lv_event_t *)
        {
            updatePowerOptions();
        }

        void loadSettingsFromRadioConfig()
        {
            const auto *board = factory::services::board();
            const auto *config = board != nullptr ? board->loraConfig() : nullptr;
            if (config == nullptr)
                return;

            static const float frequencies[] = {433.0F, 868.0F, 915.0F, 2400.0F, 2450.0F};
            static const float bandwidths[] = {125.0F, 250.0F, 500.0F};
            static const uint8_t spreading[] = {7, 8, 9, 10, 11, 12};
            static const uint8_t coding[] = {5, 6, 7, 8};
            static const int8_t low_powers[] = {-9, -6, -3, 0, 3, 5, 10, 14, 17, 20, 22};
            static const int8_t high_powers[] = {-9, -6, -3, 0, 3, 5};

            auto nearestFloatIndex = [](const float *values, std::size_t count, float value) {
                std::size_t result = 0;
                float distance = values[0] > value ? values[0] - value : value - values[0];
                for (std::size_t index = 1; index < count; ++index)
                {
                    const float candidate_distance = values[index] > value
                                                         ? values[index] - value
                                                         : value - values[index];
                    if (candidate_distance < distance)
                    {
                        result = index;
                        distance = candidate_distance;
                    }
                }
                return result;
            };
            auto byteIndex = [](const uint8_t *values, std::size_t count, uint8_t value) {
                for (std::size_t index = 0; index < count; ++index)
                    if (values[index] == value)
                        return index;
                return std::size_t{0};
            };

            s_lora.saved_settings[0] = nearestFloatIndex(frequencies, 5, config->frequency_mhz);
            s_lora.saved_settings[1] = nearestFloatIndex(bandwidths, 3, config->bandwidth_khz);
            s_lora.saved_settings[2] = byteIndex(spreading, 6, config->spreading_factor);
            s_lora.saved_settings[3] = byteIndex(coding, 4, config->coding_rate);
            const bool high_band = s_lora.saved_settings[0] >= 3U;
            const int8_t *powers = high_band ? high_powers : low_powers;
            const std::size_t power_count = high_band ? 6U : 11U;
            s_lora.saved_settings[4] = 0;
            for (std::size_t index = 0; index < power_count; ++index)
            {
                if (powers[index] == config->output_power_dbm)
                {
                    s_lora.saved_settings[4] = index;
                    break;
                }
            }
            for (std::size_t index = 0; index < kSettingCount; ++index)
            {
                if (s_lora.settings_dropdowns[index] != nullptr)
                    lv_dropdown_set_selected(s_lora.settings_dropdowns[index], s_lora.saved_settings[index]);
            }
            updatePowerOptions();
        }

        void settingsApplyClicked(lv_event_t *)
        {
            for (std::size_t index = 0; index < kSettingCount; ++index)
            {
                if (s_lora.settings_dropdowns[index] != nullptr)
                    s_lora.saved_settings[index] = lv_dropdown_get_selected(s_lora.settings_dropdowns[index]);
            }
            if (s_lora.frequency != nullptr && s_lora.settings_dropdowns[0] != nullptr)
            {
                char frequency[32]{};
                lv_dropdown_get_selected_str(s_lora.settings_dropdowns[0], frequency, sizeof(frequency));
                lv_label_set_text_fmt(s_lora.frequency, "%s  |  IDLE", frequency);
            }
            static const float frequencies[] = {433.0F, 868.0F, 915.0F, 2400.0F, 2450.0F};
            static const float bandwidths[] = {125.0F, 250.0F, 500.0F};
            static const uint8_t spreading[] = {7, 8, 9, 10, 11, 12};
            static const uint8_t coding[] = {5, 6, 7, 8};
            static const int8_t powers[] = {-9, -6, -3, 0, 3, 5, 10, 14, 17, 20, 22};
            lora_control_config_t config{};
            config.frequency_mhz = frequencies[s_lora.saved_settings[0]];
            config.bandwidth_khz = bandwidths[s_lora.saved_settings[1]];
            config.spreading_factor = spreading[s_lora.saved_settings[2]];
            config.coding_rate = coding[s_lora.saved_settings[3]];
            const std::size_t power_index = s_lora.saved_settings[0] >= 3
                                                ? std::min<std::size_t>(s_lora.saved_settings[4], 5U)
                                                : std::min<std::size_t>(s_lora.saved_settings[4], 10U);
            config.output_power_dbm = powers[power_index];
            config.preamble_length = 12;
            config.crc_enabled = true;
            (void)lora_control_apply_config(&config);
            settingsCloseClicked(nullptr);
            setStatus("Settings saved");
        }

        void loraEventTimer(lv_timer_t *)
        {
            lora_control_event_t event{};
            while (lora_control_poll_event(&event))
            {
                if (event.type == LORA_CONTROL_EVENT_TX_DONE)
                {
                    char time[16]{};
                    char power[24]{};
                    formatMessageTime(time, sizeof(time));
                    int output_power = 0;
                    if (factory::services::board() != nullptr &&
                        factory::services::board()->loraConfig() != nullptr)
                        output_power = factory::services::board()->loraConfig()->output_power_dbm;
                    std::snprintf(power, sizeof(power), "%d dBm", output_power);
                    addMessage(true, event.payload, time, power);
                    setStatus(s_lora.mode == LoraMode::timed ? "Timed transmit active" : "Transmit complete");
                }
                else if (event.type == LORA_CONTROL_EVENT_RX_PACKET)
                {
                    char time[16]{};
                    char rssi[24]{};
                    formatMessageTime(time, sizeof(time));
                    std::snprintf(rssi, sizeof(rssi), "%d dBm", static_cast<int>(event.rssi_dbm));
                    addMessage(false, event.payload, time, rssi);
                    setStatus("Packet received");
                }
                else if (event.type == LORA_CONTROL_EVENT_ERROR)
                    setStatus("LoRa radio error");
            }
        }

        void createSettingsPanel(lv_obj_t *parent)
        {
            s_lora.settings_panel = lv_obj_create(parent);
            if (s_lora.settings_panel == nullptr)
                return;
            lv_obj_set_size(s_lora.settings_panel, 440, 470);
            lv_obj_center(s_lora.settings_panel);
            stylePanel(s_lora.settings_panel, 28);
            lv_obj_set_style_bg_grad(s_lora.settings_panel, nullptr, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_lora.settings_panel, lv_color_hex(UI_BLACK), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_lora.settings_panel, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(s_lora.settings_panel, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
            lv_obj_set_style_shadow_width(s_lora.settings_panel, 28, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(s_lora.settings_panel, LV_OPA_10, LV_PART_MAIN);
            lv_obj_add_flag(s_lora.settings_panel, LV_OBJ_FLAG_HIDDEN);

            lv_obj_t *title = loraLabel(s_lora.settings_panel, "LoRa Settings",
                                        &lv_font_montserrat_16, UI_WIHTE);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 24);

            struct SettingRow
            {
                const char *name;
                const char *options;
                lv_coord_t y;
            };
            constexpr SettingRow rows[kSettingCount] = {
                {"Frequency", "433.000 MHz\n868.000 MHz\n915.000 MHz\n2400 MHz\n2450 MHz", 68},
                {"Bandwidth", "125 kHz\n250 kHz\n500 kHz", 122},
                {"Spreading", "SF7\nSF8\nSF9\nSF10\nSF11\nSF12", 176},
                {"Coding rate", "4/5\n4/6\n4/7\n4/8", 230},
                {"Power", "-9 dBm\n-6 dBm\n-3 dBm\n0 dBm\n3 dBm\n5 dBm\n10 dBm\n14 dBm\n17 dBm\n20 dBm\n22 dBm", 284},
                {"TX interval", "1 s\n2 s\n3 s\n4 s\n5 s\n6 s\n7 s\n8 s\n9 s\n10 s", 338},
            };
            for (std::size_t index = 0; index < kSettingCount; ++index)
            {
                const SettingRow &row = rows[index];
                lv_obj_t *name = loraLabel(s_lora.settings_panel, row.name,
                                           &lv_font_montserrat_16, UI_GTAY);
                lv_obj_set_width(name, 140);
                lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
                lv_obj_t *value = lv_dropdown_create(s_lora.settings_panel);
                if (value == nullptr)
                    continue;
                s_lora.settings_dropdowns[index] = value;
                if (index == 0)
                    lv_obj_add_event_cb(value, frequencyChanged, LV_EVENT_VALUE_CHANGED, nullptr);
                lv_dropdown_set_options_static(value, row.options);
                lv_dropdown_set_selected(value, s_lora.saved_settings[index]);
                lv_dropdown_set_dir(value, LV_DIR_BOTTOM);
                stylePanel(value, 12);
                lv_obj_set_size(value, 224, 42);
                lv_obj_set_pos(value, 188, row.y);
                lv_obj_align_to(name, value, LV_ALIGN_OUT_LEFT_MID, -20, 0);
                lv_obj_set_style_text_font(value, &lv_font_montserrat_16, LV_PART_MAIN);
                lv_obj_set_style_text_color(value, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(value, LV_OPA_30, LV_STATE_PRESSED);
                lv_obj_set_style_pad_left(value, 14, LV_PART_MAIN);
                lv_obj_set_style_pad_right(value, 14, LV_PART_MAIN);
                lv_obj_set_style_pad_top(value, 8, LV_PART_MAIN);

                lv_obj_t *list = lv_dropdown_get_list(value);
                if (list != nullptr)
                {
                    page_ui_apply_icon_style(list);
                    lv_obj_set_style_radius(list, 12, LV_PART_MAIN);
                    lv_obj_set_style_bg_grad(list, nullptr, LV_PART_MAIN);
                    lv_obj_set_style_bg_color(list, lv_color_hex(UI_BLACK), LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
                    lv_obj_set_style_text_font(list, &lv_font_montserrat_16, LV_PART_MAIN);
                    lv_obj_set_style_text_color(list, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                    lv_obj_set_style_max_height(list, 160, LV_PART_MAIN);
                    lv_obj_set_style_bg_color(list, lv_color_hex(UI_MAIN), LV_PART_SELECTED);
                    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SELECTED);
                    lv_obj_set_style_text_color(list, lv_color_hex(UI_WIHTE), LV_PART_SELECTED);
                    lv_obj_set_style_bg_color(list, lv_color_hex(UI_GTAY), LV_PART_SCROLLBAR);
                }
            }

            updatePowerOptions();

            lv_obj_t *close = makeButton(s_lora.settings_panel, "Cancel", 112, 44,
                                         settingsCloseClicked, nullptr);
            if (close != nullptr)
                lv_obj_align(close, LV_ALIGN_BOTTOM_LEFT, 28, -24);
            lv_obj_t *apply = makeButton(s_lora.settings_panel, "Apply", 112, 44,
                                         settingsApplyClicked, nullptr);
            if (apply != nullptr)
                lv_obj_align(apply, LV_ALIGN_BOTTOM_RIGHT, -28, -24);
        }

        void loraPageCreate(Page *page)
        {
            if (page == nullptr || page->root == nullptr)
                return;
            s_lora = LoraUi{};
            s_lora.root = page->root;
            if (factory::services::board() != nullptr &&
                factory::services::board()->initializeLora() == ESP_OK)
                lora_control_initialize();
            ui_create_background(page->root);
            lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(page->root, 0, LV_PART_MAIN);
            lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *heading = loraLabel(page->root, "LoRa",
                                          &lv_font_montserrat_24, UI_WIHTE);
            lv_obj_align(heading, LV_ALIGN_TOP_LEFT, kContentLeft, 104);
            lv_obj_t *subtitle = loraLabel(page->root, "Wireless communication",
                                           &lv_font_montserrat_16, UI_GTAY);
            lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, kContentLeft, 136);

            s_lora.settings = makeButton(page->root, LV_SYMBOL_SETTINGS, 56, 56,
                                         settingsClicked, nullptr);
            if (s_lora.settings != nullptr)
                lv_obj_align(s_lora.settings, LV_ALIGN_TOP_RIGHT, -kContentLeft, 104);

            lv_obj_t *status_panel = page_ui_container(page->root, kContentWidth, kStatusHeight);
            if (status_panel != nullptr)
            {
                lv_obj_align(status_panel, LV_ALIGN_TOP_MID, 0, kStatusTop);
                stylePanel(status_panel, 20);
                s_lora.status = loraLabel(status_panel, "LoRa UI ready",
                                          &lv_font_montserrat_16, UI_WIHTE);
                lv_obj_set_width(s_lora.status, kContentWidth - 40);
                lv_label_set_long_mode(s_lora.status, LV_LABEL_LONG_DOT);
                lv_obj_align(s_lora.status, LV_ALIGN_TOP_LEFT, 20, 10);
                s_lora.frequency = loraLabel(status_panel, "868.000 MHz  |  IDLE",
                                             &lv_font_montserrat_14, UI_GTAY);
                lv_obj_align(s_lora.frequency, LV_ALIGN_BOTTOM_LEFT, 20, -10);
            }

            s_lora.messages = page_ui_container(page->root, kContentWidth, kMessageHeight);
            if (s_lora.messages != nullptr)
            {
                lv_obj_align(s_lora.messages, LV_ALIGN_TOP_MID, 0, kMessageTop);
                stylePanel(s_lora.messages, 24);
                lv_obj_add_flag(s_lora.messages, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_scroll_dir(s_lora.messages, LV_DIR_VER);
                lv_obj_set_style_pad_all(s_lora.messages, 0, LV_PART_MAIN);
                s_lora.empty_message = loraLabel(s_lora.messages, "No messages",
                                                 &lv_font_montserrat_16, UI_GTAY);
                lv_obj_align(s_lora.empty_message, LV_ALIGN_CENTER, 0, -12);
                lv_obj_t *hint = loraLabel(s_lora.messages, "Message history will appear here",
                                           &lv_font_montserrat_16, UI_GTAY);
                s_lora.message_hint = hint;
                lv_obj_align(hint, LV_ALIGN_CENTER, 0, 22);
            }

            s_lora.input = lv_textarea_create(page->root);
            if (s_lora.input != nullptr)
            {
                lv_textarea_set_one_line(s_lora.input, true);
                lv_textarea_set_placeholder_text(s_lora.input, "Enter a LoRa message");
                stylePanel(s_lora.input, 18);
                lv_obj_set_style_text_color(s_lora.input, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                lv_obj_set_style_text_font(s_lora.input, &lv_font_montserrat_16, LV_PART_MAIN);
                lv_obj_set_style_pad_left(s_lora.input, 18, LV_PART_MAIN);
                lv_obj_set_style_pad_right(s_lora.input, 18, LV_PART_MAIN);
                lv_obj_set_style_pad_top(s_lora.input, 12, LV_PART_MAIN);
                lv_obj_set_style_pad_bottom(s_lora.input, 12, LV_PART_MAIN);
                // Set the final size after one-line mode and font/padding changes.
                lv_obj_set_size(s_lora.input, kInputWidth, kInputHeight);
                lv_obj_set_pos(s_lora.input, kContentLeft, kInputTop);
                lv_obj_add_flag(s_lora.input, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_event_cb(s_lora.input, keyboardShow, LV_EVENT_CLICKED, nullptr);
                lv_obj_add_event_cb(s_lora.input, keyboardShow, LV_EVENT_PRESSED, nullptr);
                lv_obj_add_event_cb(s_lora.input, keyboardShow, LV_EVENT_RELEASED, nullptr);
                lv_obj_add_event_cb(s_lora.input, keyboardShow, LV_EVENT_FOCUSED, nullptr);
            }

            s_lora.send = makeButton(page->root, "Send", kSendWidth, kInputHeight,
                                     sendClicked, nullptr);
            if (s_lora.send != nullptr)
                lv_obj_set_pos(s_lora.send, kContentLeft + kInputWidth + kRowGap, kInputTop);

            s_lora.keyboard = lv_keyboard_create(page->root);
            if (s_lora.keyboard != nullptr)
            {
                lv_obj_add_flag(s_lora.keyboard, LV_OBJ_FLAG_FLOATING);
                lv_obj_remove_flag(s_lora.keyboard, LV_OBJ_FLAG_SCROLL_CHAIN);
                const lv_style_selector_t keyboard_pressed = static_cast<lv_style_selector_t>(
                    static_cast<uint32_t>(LV_PART_ITEMS) |
                    static_cast<uint32_t>(LV_STATE_PRESSED));
                lv_obj_set_size(s_lora.keyboard, 560, 220);
                lv_obj_align(s_lora.keyboard, LV_ALIGN_CENTER, 0, 90);
                lv_obj_set_style_bg_color(s_lora.keyboard, lv_color_hex(0x0d0d0d), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(s_lora.keyboard, LV_OPA_COVER, LV_PART_MAIN);
                lv_obj_set_style_bg_color(s_lora.keyboard, lv_color_hex(UI_BLACK), LV_PART_ITEMS);
                lv_obj_set_style_bg_opa(s_lora.keyboard, LV_OPA_40, keyboard_pressed);
                lv_obj_set_style_text_color(s_lora.keyboard, lv_color_hex(UI_WIHTE), LV_PART_ITEMS);
                lv_obj_set_style_text_font(s_lora.keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
                lv_obj_set_style_border_width(s_lora.keyboard, 0, LV_PART_ITEMS);
                lv_obj_set_style_radius(s_lora.keyboard, 0, LV_PART_ITEMS);
                lv_obj_add_event_cb(s_lora.keyboard, keyboardEvent, LV_EVENT_READY, nullptr);
                lv_obj_add_event_cb(s_lora.keyboard, keyboardEvent, LV_EVENT_CANCEL, nullptr);
                lv_obj_add_flag(s_lora.keyboard, LV_OBJ_FLAG_HIDDEN);
            }

            s_lora.timed = makeButton(page->root, "Timed", kModeButtonWidth,
                                      kModeButtonHeight, modeClicked,
                                      reinterpret_cast<void *>(static_cast<std::uintptr_t>(LoraMode::timed)));
            if (s_lora.timed != nullptr)
                lv_obj_align(s_lora.timed, LV_ALIGN_TOP_MID, -(kModeButtonWidth + kRowGap) / 2, kModeTop);
            s_lora.receive = makeButton(page->root, "Receive", kModeButtonWidth,
                                        kModeButtonHeight, modeClicked,
                                        reinterpret_cast<void *>(static_cast<std::uintptr_t>(LoraMode::receive)));
            if (s_lora.receive != nullptr)
                lv_obj_align(s_lora.receive, LV_ALIGN_TOP_MID, (kModeButtonWidth + kRowGap) / 2, kModeTop);
            updateModeButtons();
            createSettingsPanel(page->root);
            loadSettingsFromRadioConfig();
            s_lora.event_timer = lv_timer_create(loraEventTimer, 100, nullptr);
        }

        void loraPageDestroy(Page *)
        {
            (void)lora_control_set_idle();
            lora_control_deinitialize();
            if (factory::services::board() != nullptr)
                (void)factory::services::board()->deinitializeLora();
            if (s_lora.event_timer != nullptr)
                lv_timer_del(s_lora.event_timer);
            s_lora = LoraUi{};
        }

        void loraPageLeave(Page *)
        {
            (void)ui_page_destroy(PAGE_LORA);
        }
    } // namespace

    void registerLoraPage()
    {
        Page page{};
        page.id = PAGE_LORA;
        page.name = "lora";
        page.on_create = loraPageCreate;
        page.on_destroy = loraPageDestroy;
        page.on_leave = loraPageLeave;
        page.on_gesture = page_ui_back_gesture;
        (void)ui_page_register(&page);
    }
} // namespace factory::ui

extern "C" void lora_page_register(void)
{
    factory::ui::registerLoraPage();
}
