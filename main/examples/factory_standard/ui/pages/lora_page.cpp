#include "page_common.h"
#include <app/services/lora_control.h>
#include <app/services/service_context.hpp>

#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

#define LORA_W 720
#define LORA_H 720
#define LORA_MARGIN 28
#define LORA_CONTENT_Y 58
#define LORA_BOTTOM_MARGIN 28
#define LORA_DIALOG_HEIGHT (LORA_H - LORA_CONTENT_Y - LORA_BOTTOM_MARGIN)
#define LORA_INPUT_Y 574
#define LORA_MODE_Y 648
#define LORA_INPUT_HEIGHT 56
#define LORA_KEYBOARD_HEIGHT (LORA_H / 2)
#define LORA_KEYBOARD_INPUT_Y (LORA_H - LORA_KEYBOARD_HEIGHT - LORA_INPUT_HEIGHT - 12)

static constexpr uint32_t LORA_DEFAULT_TIMED_INTERVAL_INDEX = 1U;

typedef enum
{
    LORA_MODE_MANUAL,
    LORA_MODE_TIMED,
    LORA_MODE_RECEIVE,
} lora_mode_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *chat;
    lv_obj_t *status;
    lv_obj_t *meter;
    lv_obj_t *input;
    lv_obj_t *send;
    lv_obj_t *keyboard;
    lv_obj_t *mode_buttons[2];
    lv_obj_t *settings_panel;
    lv_obj_t *settings_dropdowns[6];
    lv_obj_t *message_bubbles[5];
    lv_timer_t *event_timer;
    lora_mode_t mode;
    unsigned message_count;
} lora_page_ui_t;

static lora_page_ui_t s_lora_ui;

static void lora_style_panel(lv_obj_t *object, uint32_t color, uint32_t border,
                             lv_coord_t radius)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

static void lora_style_button(lv_obj_t *button, uint32_t color, uint32_t border)
{
    lora_style_panel(button, color, border, 0);
    lv_obj_set_style_border_opa(button, LV_OPA_40, 0);
}

static lv_obj_t *lora_text(lv_obj_t *parent, const char *value,
                           const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void lora_keyboard_hide(void)
{
    if (s_lora_ui.keyboard == NULL)
        return;

    lv_obj_add_flag(s_lora_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_lora_ui.keyboard, NULL);
    lv_obj_set_y(s_lora_ui.input, LORA_INPUT_Y);
    lv_obj_set_y(s_lora_ui.send, LORA_INPUT_Y);
}

static void lora_keyboard_show(void)
{
    if (s_lora_ui.keyboard == NULL ||
        lv_obj_has_state(s_lora_ui.input, LV_STATE_DISABLED))
        return;

    s_lora_ui.mode = LORA_MODE_MANUAL;
    (void)lora_control_set_idle();
    lv_obj_set_y(s_lora_ui.input, LORA_KEYBOARD_INPUT_Y);
    lv_obj_set_y(s_lora_ui.send, LORA_KEYBOARD_INPUT_Y);
    lv_keyboard_set_textarea(s_lora_ui.keyboard, s_lora_ui.input);
    lv_obj_clear_flag(s_lora_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lora_ui.input);
    lv_obj_move_foreground(s_lora_ui.send);
    lv_obj_move_foreground(s_lora_ui.keyboard);
}

static void lora_add_message(bool tx, const char *message, const char *time,
                             const char *rssi)
{
    if (s_lora_ui.chat == NULL)
        return;
    unsigned slot;
    if (s_lora_ui.message_count < 5U)
    {
        slot = s_lora_ui.message_count;
    }
    else
    {
        lv_obj_delete(s_lora_ui.message_bubbles[0]);
        for (unsigned index = 1; index < 5U; ++index)
        {
            s_lora_ui.message_bubbles[index - 1U] = s_lora_ui.message_bubbles[index];
            lv_obj_set_y(s_lora_ui.message_bubbles[index - 1U],
                         34 + (lv_coord_t)(index - 1U) * 78);
        }
        s_lora_ui.message_bubbles[4] = NULL;
        slot = 4;
    }
    const lv_coord_t row_y = 34 + (lv_coord_t)slot * 78;
    const lv_coord_t bubble_w = tx ? 390 : 430;
    const lv_coord_t bubble_x = tx ? 664 - bubble_w - 18 : 18;
    if (s_lora_ui.message_bubbles[slot] != NULL)
        lv_obj_delete(s_lora_ui.message_bubbles[slot]);
    lv_obj_t *bubble = lv_obj_create(s_lora_ui.chat);
    s_lora_ui.message_bubbles[slot] = bubble;
    lv_obj_set_size(bubble, bubble_w, 62);
    lv_obj_set_pos(bubble, bubble_x, row_y);
    lora_style_panel(bubble, tx ? 0x18252D : 0x11231F,
                     tx ? CYBER_MAGENTA : CYBER_GREEN, 2);
    lv_obj_set_style_border_width(bubble, 1, 0);
    char header[64];
    snprintf(header, sizeof(header), "%s   %s%s", tx ? "TX / LOCAL" : "RX / NODE-02",
             time, tx ? "" : "   ");
    lv_obj_t *head = lora_text(bubble, header, &lv_font_ShareTechMono1_16,
                               tx ? CYBER_MAGENTA : CYBER_GREEN);
    lv_obj_set_pos(head, 14, 8);
    lv_obj_t *body = lora_text(bubble, message, &lv_font_ShareTechMono1_20,
                               CYBER_TEXT);
    lv_obj_set_width(body, bubble_w - 28);
    lv_label_set_long_mode(body, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(body, 14, 29);
    if (!tx && rssi != NULL)
    {
        lv_obj_t *signal = lora_text(bubble, rssi, &lv_font_ShareTechMono1_16,
                                     CYBER_YELLOW);
        lv_obj_align(signal, LV_ALIGN_TOP_RIGHT, -12, 8);
    }
    ++s_lora_ui.message_count;
}

static void lora_mode_refresh(void)
{
    static const char *names[] = {"TIMED", "RECEIVE"};
    static const lora_mode_t modes[] = {LORA_MODE_TIMED, LORA_MODE_RECEIVE};
    for (int i = 0; i < 2; ++i)
    {
        lv_obj_t *button = s_lora_ui.mode_buttons[i];
        if (button == NULL)
            continue;
        const bool active = s_lora_ui.mode == modes[i];
        lv_obj_set_style_bg_color(button, lv_color_hex(active ? CYBER_CYAN : CYBER_PANEL), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                    lv_color_hex(active ? CYBER_BG : CYBER_MUTED), 0);
        lv_label_set_text(lv_obj_get_child(button, 0), names[i]);
    }
    if (s_lora_ui.input != NULL)
        lv_obj_clear_state(s_lora_ui.input, LV_STATE_DISABLED);
    if (s_lora_ui.send != NULL)
        lv_obj_clear_state(s_lora_ui.send, LV_STATE_DISABLED);
}

static void lora_mode_cb(lv_event_t *event)
{
    lora_keyboard_hide();
    const lora_mode_t requested = (lora_mode_t)(uintptr_t)lv_event_get_user_data(event);
    if (s_lora_ui.mode == requested)
    {
        if (lora_control_set_idle())
        {
            s_lora_ui.mode = LORA_MODE_MANUAL;
            lv_label_set_text(s_lora_ui.status, "LORA LINK // ONLINE");
            lora_mode_refresh();
        }
        else
        {
            lv_label_set_text(s_lora_ui.status, "MODE // STOP FAILED");
        }
        return;
    }

    bool accepted = false;
    if (requested == LORA_MODE_TIMED)
    {
        static const uint32_t intervals_ms[] = {500, 1000, 1500, 2000, 3000, 5000};
        const uint32_t selected = lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[5]);
        accepted = lora_control_start_timed(intervals_ms[selected]);
        if (!accepted)
            lv_label_set_text(s_lora_ui.status, "TIMED // START FAILED");
    }
    else if (requested == LORA_MODE_RECEIVE)
    {
        accepted = lora_control_start_receive();
        if (!accepted)
            lv_label_set_text(s_lora_ui.status, "RECEIVE // START FAILED");
    }
    if (accepted)
    {
        s_lora_ui.mode = requested;
        lv_label_set_text(s_lora_ui.status,
                          requested == LORA_MODE_TIMED ? "TIMED TX // ACTIVE" : "RECEIVE // LISTENING");
    }
    lora_mode_refresh();
}

static void lora_send_cb(lv_event_t *event)
{
    (void)event;
    const char *value = lv_textarea_get_text(s_lora_ui.input);
    if (value == NULL || value[0] == '\0')
        return;
    if (lora_control_send(value))
    {
        s_lora_ui.mode = LORA_MODE_MANUAL;
        lv_label_set_text(s_lora_ui.status, "TX // SENDING");
        lv_textarea_set_text(s_lora_ui.input, "");
        lora_keyboard_hide();
        lora_mode_refresh();
    }
    else
    {
        lv_label_set_text(s_lora_ui.status, "TX // QUEUE BUSY");
    }
}

static void lora_input_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED)
    {
        lora_keyboard_show();
        lora_mode_refresh();
    }
    else if (code == LV_EVENT_READY)
    {
        lora_send_cb(event);
    }
    else if (code == LV_EVENT_CANCEL)
    {
        lora_keyboard_hide();
    }
}

static void lora_settings_close_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_lora_ui.settings_panel, LV_OBJ_FLAG_HIDDEN);
}

static void lora_settings_open_cb(lv_event_t *event)
{
    (void)event;
    lora_keyboard_hide();
    lv_obj_clear_flag(s_lora_ui.settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lora_ui.settings_panel);
}

static void lora_apply_settings_cb(lv_event_t *event)
{
    (void)event;
    static const float frequencies_mhz[] = {433.0F, 868.0F, 915.0F, 2400.0F, 2450.0F};
    static const float bandwidths_khz[] = {125.0F, 250.0F, 500.0F};
    static const uint8_t spreading_factors[] = {7, 8, 9, 10, 11, 12};
    static const uint8_t coding_rates[] = {5, 6, 7, 8};
    static const int8_t low_band_powers_dbm[] = {-9, -6, -3, 0, 3, 5, 10, 14, 17, 20, 22};
    static const int8_t high_band_powers_dbm[] = {-9, -6, -3, 0, 3, 5};
    const uint32_t frequency_index = lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[0]);
    const bool high_band = frequency_index >= 3U;
    const int8_t *output_powers_dbm = high_band ? high_band_powers_dbm : low_band_powers_dbm;

    lora_control_config_t config = {
        .frequency_mhz = frequencies_mhz[lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[0])],
        .bandwidth_khz = bandwidths_khz[lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[1])],
        .spreading_factor = spreading_factors[lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[2])],
        .coding_rate = coding_rates[lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[3])],
        .output_power_dbm = output_powers_dbm[lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[4])],
        .preamble_length = 12,
        .crc_enabled = true,
    };
    if (lora_control_apply_config(&config))
    {
        lv_label_set_text(s_lora_ui.status, "RADIO CONFIG // APPLYING");
        lv_obj_add_flag(s_lora_ui.settings_panel, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text(s_lora_ui.status, "RADIO CONFIG // QUEUE BUSY");
    }
}

static void lora_format_time(char *buffer, size_t size)
{
    const uint64_t seconds = (uint64_t)(esp_timer_get_time() / 1000000LL);
    snprintf(buffer, size, "%02llu:%02llu:%02llu",
             (unsigned long long)((seconds / 3600U) % 100U),
             (unsigned long long)((seconds / 60U) % 60U),
             (unsigned long long)(seconds % 60U));
}

static const char *lora_frequency_text(float frequency_mhz)
{
    if (frequency_mhz >= 2449.5F)
        return "2450 MHz";
    if (frequency_mhz >= 2399.5F)
        return "2400 MHz";
    if (frequency_mhz >= 914.5F)
        return "915.000 MHz";
    if (frequency_mhz >= 867.5F)
        return "868.000 MHz";
    return "433.000 MHz";
}

static void lora_event_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    lora_control_event_t event;
    while (lora_control_poll_event(&event))
    {
        char time[16];
        lora_format_time(time, sizeof(time));
        switch (event.type)
        {
        case LORA_CONTROL_EVENT_TX_DONE:
            lora_add_message(true, event.payload, time, NULL);
            lv_label_set_text(s_lora_ui.status,
                              s_lora_ui.mode == LORA_MODE_TIMED ? "TIMED TX // ACTIVE" : "TX // COMPLETE");
            break;
        case LORA_CONTROL_EVENT_RX_PACKET:
        {
            char signal[40];
            const int rssi = (int)(event.rssi_dbm < 0.0F ? event.rssi_dbm - 0.5F : event.rssi_dbm + 0.5F);
            const int snr = (int)(event.snr_db < 0.0F ? event.snr_db - 0.5F : event.snr_db + 0.5F);
            snprintf(signal, sizeof(signal), "%d dBm", rssi);
            lora_add_message(false, event.payload, time, signal);
            lv_label_set_text_fmt(s_lora_ui.status, "RX // PACKET  /  SNR %d dB", snr);
            lv_label_set_text_fmt(s_lora_ui.meter, "%s   %d dBm",
                                  lora_frequency_text(event.config.frequency_mhz), rssi);
            break;
        }
        case LORA_CONTROL_EVENT_CONFIG_APPLIED:
            lv_label_set_text(s_lora_ui.status, "RADIO CONFIG // APPLIED");
            lv_label_set_text_fmt(s_lora_ui.meter, "%s   READY",
                                  lora_frequency_text(event.config.frequency_mhz));
            break;
        case LORA_CONTROL_EVENT_ERROR:
            lv_label_set_text_fmt(s_lora_ui.status, "ERROR // %s  (%ld)",
                                  event.payload, (long)event.code);
            break;
        }
    }
}

static void lora_frequency_changed_cb(lv_event_t *event);

static lv_obj_t *lora_dropdown(lv_obj_t *parent, const char *caption,
                               const char *options, lv_coord_t y, int index)
{
    lv_obj_t *label = lora_text(parent, caption, &lv_font_ShareTechMono1_16, CYBER_MUTED);
    lv_obj_set_pos(label, 28, y + 13);
    lv_obj_t *drop = lv_dropdown_create(parent);
    lv_dropdown_set_options(drop, options);
    lv_obj_set_size(drop, 270, 42);
    lv_obj_set_pos(drop, 260, y);
    lv_obj_set_style_bg_color(drop, lv_color_hex(CYBER_PANEL), 0);
    lv_obj_set_style_bg_opa(drop, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(drop, lv_color_hex(CYBER_TEXT), 0);
    lv_obj_set_style_text_font(drop, &lv_font_ShareTechMono1_16, 0);
    lv_obj_set_style_border_color(drop, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_border_width(drop, 1, 0);
    lv_obj_set_style_border_opa(drop, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(drop, 0, 0);
    lv_obj_set_style_pad_left(drop, 12, 0);

    lv_obj_t *list = lv_dropdown_get_list(drop);
    const lv_style_selector_t selected_pressed = static_cast<lv_style_selector_t>(
        static_cast<uint32_t>(LV_PART_SELECTED) |
        static_cast<uint32_t>(LV_STATE_PRESSED));
    lv_obj_set_style_bg_color(list, lv_color_hex(CYBER_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(CYBER_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &lv_font_ShareTechMono1_16, LV_PART_MAIN);
    lv_obj_set_style_border_color(list, lv_color_hex(CYBER_CYAN), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(list, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(CYBER_CYAN), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_hex(CYBER_BG), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, lv_color_hex(CYBER_MAGENTA),
                              selected_pressed);
    lv_obj_set_style_bg_opa(list, LV_OPA_40,
                            selected_pressed);
    lv_obj_set_style_text_color(list, lv_color_hex(CYBER_BG),
                                selected_pressed);
    lv_obj_set_style_bg_color(list, lv_color_hex(CYBER_CYAN), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 0, LV_PART_SCROLLBAR);
    if (index == 0)
        lv_obj_add_event_cb(drop, lora_frequency_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    s_lora_ui.settings_dropdowns[index] = drop;
    return drop;
}

static void lora_frequency_changed_cb(lv_event_t *event)
{
    (void)event;
    if (s_lora_ui.settings_dropdowns[4] == NULL)
        return;
    const bool high_band = lv_dropdown_get_selected(s_lora_ui.settings_dropdowns[0]) >= 3U;
    if (high_band)
    {
        lv_dropdown_set_options_static(s_lora_ui.settings_dropdowns[4],
                                       "-9 dBm\n-6 dBm\n-3 dBm\n0 dBm\n3 dBm\n5 dBm");
        lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[4], 5);
    }
    else
    {
        lv_dropdown_set_options_static(s_lora_ui.settings_dropdowns[4],
                                       "-9 dBm\n-6 dBm\n-3 dBm\n0 dBm\n3 dBm\n5 dBm\n10 dBm\n14 dBm\n17 dBm\n20 dBm\n22 dBm");
        lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[4], 10);
    }
}

static void lora_page_create(Page *page)
{
    memset(&s_lora_ui, 0, sizeof(s_lora_ui));
    s_lora_ui.root = page->root;
    s_lora_ui.mode = LORA_MODE_MANUAL;
    lv_obj_set_style_bg_color(page->root, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_clear_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);

    s_lora_ui.chat = lv_obj_create(page->root);
    lv_obj_set_size(s_lora_ui.chat, 664, 500);
    lv_obj_set_pos(s_lora_ui.chat, LORA_MARGIN, LORA_CONTENT_Y);
    lora_style_panel(s_lora_ui.chat, CYBER_PANEL, CYBER_CYAN, 0);
    lv_obj_set_style_border_width(s_lora_ui.chat, 0, 0);
    lv_obj_set_style_shadow_color(s_lora_ui.chat, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_shadow_width(s_lora_ui.chat, 12, 0);
    lv_obj_set_style_shadow_opa(s_lora_ui.chat, LV_OPA_20, 0);
    page_ui_add_corner_brackets(s_lora_ui.chat, 0, 0, 664, 500,
                                CYBER_YELLOW, 6);

    s_lora_ui.status = lora_text(s_lora_ui.chat, "LORA // INITIALIZING",
                                 &lv_font_ShareTechMono1_16, CYBER_CYAN);
    lv_obj_set_pos(s_lora_ui.status, 18, 12);
    s_lora_ui.meter = lora_text(s_lora_ui.chat, "868.000 MHz   READY",
                                &lv_font_ShareTechMono1_16, CYBER_YELLOW);
    lv_obj_align(s_lora_ui.meter, LV_ALIGN_TOP_RIGHT, -18, 12);

    s_lora_ui.input = lv_textarea_create(page->root);
    lv_obj_set_size(s_lora_ui.input, 500, LORA_INPUT_HEIGHT);
    lv_obj_set_pos(s_lora_ui.input, LORA_MARGIN, LORA_INPUT_Y);
    lv_textarea_set_one_line(s_lora_ui.input, true);
    lv_textarea_set_placeholder_text(s_lora_ui.input, "TYPE MESSAGE...");
    lv_obj_set_style_bg_color(s_lora_ui.input, lv_color_hex(CYBER_PANEL), 0);
    lv_obj_set_style_text_color(s_lora_ui.input, lv_color_hex(CYBER_TEXT), 0);
    lv_obj_set_style_text_font(s_lora_ui.input, &lv_font_ShareTechMono1_20, 0);
    lv_obj_set_style_border_color(s_lora_ui.input, lv_color_hex(CYBER_MAGENTA), 0);
    lv_obj_set_style_border_width(s_lora_ui.input, 1, 0);
    lv_obj_set_style_radius(s_lora_ui.input, 0, 0);
    lv_obj_add_event_cb(s_lora_ui.input, lora_input_cb, LV_EVENT_ALL, NULL);

    s_lora_ui.send = page_ui_button(page->root, LV_SYMBOL_UP, "SEND", 120,
                                    LORA_INPUT_HEIGHT,
                                    lora_send_cb, NULL);
    lv_obj_set_pos(s_lora_ui.send, 572, LORA_INPUT_Y);
    lora_style_button(s_lora_ui.send, CYBER_MAGENTA, CYBER_MAGENTA);
    lv_obj_set_height(s_lora_ui.send, lv_obj_get_height(s_lora_ui.input));
    lv_obj_set_style_text_color(lv_obj_get_child(s_lora_ui.send, 0), lv_color_hex(CYBER_BG), 0);

    const char *mode_names[] = {"TIMED SEND", "RECEIVE"};
    const lora_mode_t modes[] = {LORA_MODE_TIMED, LORA_MODE_RECEIVE};
    for (int i = 0; i < 2; ++i)
    {
        s_lora_ui.mode_buttons[i] = page_ui_button(page->root, NULL, mode_names[i],
                                                   180, 44, lora_mode_cb,
                                                   (void *)(uintptr_t)modes[i]);
        lv_obj_set_pos(s_lora_ui.mode_buttons[i], LORA_MARGIN + i * 188, LORA_MODE_Y);
        lora_style_button(s_lora_ui.mode_buttons[i], CYBER_PANEL, CYBER_CYAN);
        lv_obj_set_style_text_font(lv_obj_get_child(s_lora_ui.mode_buttons[i], 0),
                                   &lv_font_ShareTechMono1_16, 0);
    }
    lv_obj_t *settings = page_ui_button(page->root, LV_SYMBOL_SETTINGS, NULL, 120, 44,
                                        lora_settings_open_cb, NULL);
    lv_obj_set_pos(settings, 572, LORA_MODE_Y);
    lora_style_button(settings, CYBER_PANEL, CYBER_YELLOW);
    lv_obj_set_style_text_color(lv_obj_get_child(settings, 0), lv_color_hex(CYBER_YELLOW), 0);
    lora_mode_refresh();

    s_lora_ui.keyboard = lv_keyboard_create(page->root);
    const lv_style_selector_t keyboard_pressed = static_cast<lv_style_selector_t>(
        static_cast<uint32_t>(LV_PART_ITEMS) |
        static_cast<uint32_t>(LV_STATE_PRESSED));
    lv_obj_set_size(s_lora_ui.keyboard, LORA_W, LORA_KEYBOARD_HEIGHT);
    lv_obj_align(s_lora_ui.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_lora_ui.keyboard, lv_color_hex(CYBER_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_lora_ui.keyboard, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_lora_ui.keyboard, lv_color_hex(CYBER_CYAN), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_lora_ui.keyboard, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_lora_ui.keyboard, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_lora_ui.keyboard, lv_color_hex(CYBER_PANEL), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_lora_ui.keyboard, lv_color_hex(CYBER_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_lora_ui.keyboard, &lv_font_ShareTechMono1_16, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_lora_ui.keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_lora_ui.keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_lora_ui.keyboard, lv_color_hex(CYBER_MAGENTA),
                              keyboard_pressed);
    lv_obj_set_style_bg_opa(s_lora_ui.keyboard, LV_OPA_40,
                            keyboard_pressed);
    lv_obj_add_flag(s_lora_ui.keyboard, LV_OBJ_FLAG_HIDDEN);

    s_lora_ui.settings_panel = lv_obj_create(page->root);
    lv_obj_set_size(s_lora_ui.settings_panel, 664, LORA_DIALOG_HEIGHT);
    lv_obj_set_pos(s_lora_ui.settings_panel, LORA_MARGIN, LORA_CONTENT_Y);
    lora_style_panel(s_lora_ui.settings_panel, CYBER_PANEL, CYBER_YELLOW, 0);
    lv_obj_set_style_border_width(s_lora_ui.settings_panel, 0, 0);
    lv_obj_set_style_radius(s_lora_ui.settings_panel, 0, 0);
    page_ui_add_top_left_bottom_right_brackets(s_lora_ui.settings_panel, 664,
                                               LORA_DIALOG_HEIGHT,
                                               CYBER_YELLOW, 6);
    lv_obj_add_flag(s_lora_ui.settings_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *heading = lora_text(s_lora_ui.settings_panel, "RADIO CONFIG", &lv_font_ShareTechMono1_24,
                                  CYBER_YELLOW);
    lv_obj_set_pos(heading, 28, 22);
    lv_obj_t *close = page_ui_button(s_lora_ui.settings_panel, LV_SYMBOL_CLOSE, NULL, 48, 42,
                                     lora_settings_close_cb, NULL);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -18, 18);
    lv_obj_set_style_border_opa(close, LV_OPA_40, 0);
    lora_dropdown(s_lora_ui.settings_panel, "FREQUENCY",
                  "433.000 MHz\n868.000 MHz\n915.000 MHz\n2400 MHz\n2450 MHz", 72, 0);
    lora_dropdown(s_lora_ui.settings_panel, "BANDWIDTH", "125 kHz\n250 kHz\n500 kHz", 124, 1);
    lora_dropdown(s_lora_ui.settings_panel, "SPREADING FACTOR", "SF7\nSF8\nSF9\nSF10\nSF11\nSF12", 176, 2);
    lora_dropdown(s_lora_ui.settings_panel, "CODING RATE", "4/5\n4/6\n4/7\n4/8", 228, 3);
    lora_dropdown(s_lora_ui.settings_panel, "OUTPUT POWER",
                  "-9 dBm\n-6 dBm\n-3 dBm\n0 dBm\n3 dBm\n5 dBm\n10 dBm\n14 dBm\n17 dBm\n20 dBm\n22 dBm", 280, 4);
    lora_dropdown(s_lora_ui.settings_panel, "TIMED INTERVAL", "500 ms\n1000 ms\n1500 ms\n2000 ms\n3000 ms\n5000 ms", 332, 5);
    lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[0], 1);
    lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[1], 0);
    lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[2], 0);
    lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[3], 0);
    lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[4], 10);
    lv_dropdown_set_selected(s_lora_ui.settings_dropdowns[5],
                             LORA_DEFAULT_TIMED_INTERVAL_INDEX);
    lv_obj_t *apply = page_ui_button(s_lora_ui.settings_panel, LV_SYMBOL_OK, "APPLY", 180, 48,
                                     lora_apply_settings_cb, NULL);
    lv_obj_align(apply, LV_ALIGN_BOTTOM_RIGHT, -24, -24);
    lora_style_button(apply, CYBER_GREEN, CYBER_GREEN);
    lv_obj_set_style_text_color(lv_obj_get_child(apply, 0), lv_color_hex(CYBER_BG), 0);

    s_lora_ui.event_timer = lv_timer_create(lora_event_timer_cb, 100, NULL);
}

static void lora_page_enter(Page *page)
{
    (void)page;
    bool initialized = false;
    if (factory::services::board() != nullptr)
        initialized = factory::services::board()->initializeLora() == ESP_OK;
    if (initialized)
        lora_control_initialize();
    if (s_lora_ui.event_timer != NULL)
        lv_timer_resume(s_lora_ui.event_timer);
    lv_label_set_text(s_lora_ui.status,
                      initialized && lora_control_is_ready() ? "LORA LINK // ONLINE" : "LORA LINK // UNAVAILABLE");
}

static void lora_page_leave(Page *page)
{
    (void)page;
    (void)lora_control_set_idle();
    lora_control_deinitialize();
    if (factory::services::board() != nullptr)
        (void)factory::services::board()->deinitializeLora();
    if (s_lora_ui.event_timer != NULL)
        lv_timer_pause(s_lora_ui.event_timer);
    (void)ui_page_destroy(PAGE_LORA);
}

static void lora_page_destroy(Page *page)
{
    (void)page;
    (void)lora_control_set_idle();
    if (s_lora_ui.event_timer != NULL)
        lv_timer_delete(s_lora_ui.event_timer);
    memset(&s_lora_ui, 0, sizeof(s_lora_ui));
}

extern "C" void lora_page_register(void)
{
    Page page{};

    page.id = PAGE_LORA;

    page.name = "lora";

    page.on_create = lora_page_create;

    page.on_destroy = lora_page_destroy;

    page.on_enter = lora_page_enter;

    page.on_leave = lora_page_leave;

    page.on_gesture = page_ui_back_gesture;
    ui_page_register(&page);
}
