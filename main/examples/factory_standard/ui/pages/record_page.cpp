#include "page_common.h"
#include <app/services/record_audio.h>
#include <app/services/music_audio.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <esp_vfs_fat.h>

#define RECORD_SCREEN_WIDTH 720
#define RECORD_SCREEN_HEIGHT 720
#define RECORD_SIDE_MARGIN 28
#define RECORD_CONTENT_WIDTH (RECORD_SCREEN_WIDTH - RECORD_SIDE_MARGIN * 2)
#define RECORD_WAVE_BARS 64
#define RECORD_WAVE_SCROLL_STEP 4
#define RECORD_MAX_FILES 3
#define RECORD_DB_UPDATE_MS 100
#define RECORD_DIRECTORY "/sdcard/record"
#define RECORD_FILE_LIMIT 64
#define RECORD_ROW_CACHE 5
#define RECORD_FILE_NAME_SIZE 96

static lv_obj_t *s_record_left_db;
static lv_obj_t *s_record_right_db;
static lv_obj_t *s_record_left_level;
static lv_obj_t *s_record_right_level;
static lv_obj_t *s_record_button_label;
static lv_obj_t *s_record_elapsed;
static lv_obj_t *s_record_cache_size;
static lv_obj_t *s_record_volume_text;
static lv_obj_t *s_record_storage;
static lv_obj_t *s_record_record_text;
static lv_obj_t *s_record_play_text;
static lv_obj_t *s_record_pause_button_label;
static lv_obj_t *s_record_wave_bars[RECORD_WAVE_BARS];
static uint8_t s_record_wave_heights[RECORD_WAVE_BARS];
static lv_timer_t *s_record_db_timer;
static bool s_record_page_active;

typedef struct
{
    char names[RECORD_FILE_LIMIT][RECORD_FILE_NAME_SIZE];
    size_t count;
    lv_obj_t *viewport;
    lv_obj_t *rows[RECORD_ROW_CACHE];
    lv_obj_t *numbers[RECORD_ROW_CACHE];
    lv_obj_t *labels[RECORD_ROW_CACHE];
    lv_obj_t *durations[RECORD_ROW_CACHE];
    uint32_t durations_ms[RECORD_FILE_LIMIT];
} record_list_ui_t;

static record_list_ui_t s_record_list;

static void record_gesture_passthrough(lv_obj_t *object)
{
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static lv_obj_t *record_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t width, lv_coord_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, width, height);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(CYBER_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(CYBER_GRID), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    record_gesture_passthrough(panel);
    return panel;
}

static lv_obj_t *record_icon_button(lv_obj_t *parent, const char *symbol,
                                    lv_coord_t x, uint32_t color,
                                    bool primary, lv_event_cb_t callback)
{
    lv_obj_t *button = page_ui_icon_button(parent, symbol,
                                           primary ? 72 : 60,
                                           primary ? 72 : 60,
                                           callback, NULL);
    lv_obj_set_pos(button, x, primary ? 0 : 6);
    lv_obj_set_style_bg_color(button,
                              primary ? lv_color_hex(color) : lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_bg_opa(button, primary ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_shadow_width(button, primary ? 14 : 6, 0);
    lv_obj_set_style_shadow_spread(button, primary ? 2 : 0, 0);
    lv_obj_set_style_shadow_opa(button, primary ? LV_OPA_40 : LV_OPA_20, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(primary ? CYBER_BG : CYBER_TEXT), 0);
    return button;
}

static lv_obj_t *record_level_meter(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                    uint32_t color, lv_coord_t level_width)
{
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_set_size(track, 296, 5);
    lv_obj_set_pos(track, x, y);
    lv_obj_set_style_bg_color(track, lv_color_hex(CYBER_GRID), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_80, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    record_gesture_passthrough(track);

    lv_obj_t *level = lv_obj_create(track);
    lv_obj_set_size(level, level_width, 5);
    lv_obj_set_pos(level, 0, 0);
    lv_obj_set_style_bg_color(level, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(level, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(level, 0, 0);
    lv_obj_set_style_radius(level, 0, 0);
    lv_obj_set_style_pad_all(level, 0, 0);
    lv_obj_set_style_shadow_color(level, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(level, 6, 0);
    lv_obj_set_style_shadow_opa(level, LV_OPA_30, 0);
    record_gesture_passthrough(level);
    return level;
}

static void record_waveform_channel(lv_obj_t *parent, lv_coord_t start_x,
                                    lv_coord_t center_y, uint32_t primary_color,
                                    uint32_t accent_color, uint32_t tertiary_color,
                                    uint8_t phase)
{
    for (size_t index = 0U; index < RECORD_WAVE_BARS; ++index)
    {
        // Keep all bars at the same baseline until live audio levels are available.
        const lv_coord_t height = 12;
        const uint32_t color = (index + phase) % 3U == 0U
                                   ? primary_color
                                   : (index + phase) % 3U == 1U ? accent_color
                                                               : tertiary_color;
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_set_size(bar, 2, height);
        // The waveform coordinates are relative to the wave panel.  Using
        // LV_ALIGN_LEFT_MID here applies the y offset from the panel's center,
        // which places the bars outside the intended waveform area.
        lv_obj_set_pos(bar, start_x + (lv_coord_t)(index * 5U),
                       center_y - height / 2);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(bar, index % 3U == 0U ? LV_OPA_70 : LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_shadow_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_width(bar, 5, 0);
        lv_obj_set_style_shadow_opa(bar, LV_OPA_20, 0);
        record_gesture_passthrough(bar);
        s_record_wave_bars[index] = bar;
    }
}

static void record_waveform_update(void)
{
    const bool record_playing = record_audio_is_recording() || record_audio_is_playing();
    const bool music_playing = music_audio_is_playing();
    const bool music_paused = music_playing && music_audio_is_paused();
    if (!record_playing && !music_playing)
        return;

    uint8_t levels[RECORD_WAVE_BARS]{};
    if (record_playing)
    {
        record_audio_get_waveform(levels, RECORD_WAVE_BARS);

        for (size_t index = 0U; index < RECORD_WAVE_BARS; ++index)
        {
            const uint8_t target = levels[index] > 33U ? 100U : (uint8_t)(levels[index] * 3U);
            const uint8_t previous = s_record_wave_heights[index];
            s_record_wave_heights[index] = target > previous
                                               ? (uint8_t)((previous * 2U + target * 3U) / 5U)
                                               : (uint8_t)((previous * 4U + target) / 5U);
        }
    }
    else
    {
        int16_t samples[RECORD_WAVE_BARS]{};
        music_audio_get_waveform(samples, RECORD_WAVE_BARS);
        for (size_t index = 0U; index < RECORD_WAVE_BARS; ++index)
        {
            const int32_t sample = samples[index];
            const uint32_t magnitude = sample == INT16_MIN
                                           ? 32768U
                                           : static_cast<uint32_t>(sample < 0 ? -sample : sample);
            const uint32_t level = magnitude * 100U / 32768U;
            levels[index] = static_cast<uint8_t>(level > 100U ? 100U : level);
        }
        memmove(s_record_wave_heights,
                s_record_wave_heights + RECORD_WAVE_SCROLL_STEP,
                (RECORD_WAVE_BARS - RECORD_WAVE_SCROLL_STEP) * sizeof(s_record_wave_heights[0]));
        for (size_t step = 0U; step < RECORD_WAVE_SCROLL_STEP; ++step)
        {
            const size_t sample_index = step * (RECORD_WAVE_BARS - 1U) /
                                        (RECORD_WAVE_SCROLL_STEP - 1U);
            const size_t bar_index = RECORD_WAVE_BARS - RECORD_WAVE_SCROLL_STEP + step;
            const uint8_t level = levels[sample_index];
            const uint8_t target = music_paused
                                       ? 0U
                                       : (level > 33U ? 100U : (uint8_t)(level * 3U));
            const uint8_t previous = bar_index == 0U ? 0U : s_record_wave_heights[bar_index - 1U];
            s_record_wave_heights[bar_index] = target > previous
                                                   ? (uint8_t)((previous * 2U + target * 3U) / 5U)
                                                   : (uint8_t)((previous * 4U + target) / 5U);
        }
    }

    for (size_t index = 0U; index < RECORD_WAVE_BARS; ++index)
    {
        const uint8_t smoothed = s_record_wave_heights[index];
        const lv_coord_t height = (lv_coord_t)(6U + smoothed * 104U / 100U);
        lv_obj_set_height(s_record_wave_bars[index], height);
        lv_obj_set_y(s_record_wave_bars[index], 153 - height / 2);
    }
}

static void record_microphone_header(lv_obj_t *parent, lv_coord_t x,
                                     const char *channel, const char *db,
                                     uint32_t color)
{
    lv_obj_t *name = page_ui_label(parent, channel,
                                   &lv_font_ShareTechMono1_16, color);
    lv_obj_set_pos(name, x, 42);

    lv_obj_t *value = page_ui_label(parent, db,
                                    &lv_font_ShareTechMono1_24, CYBER_TEXT);
    lv_obj_set_width(value, 122);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(value, x + 174, 35);
    if (x < 100)
        s_record_left_db = value;
    else
        s_record_right_db = value;

    lv_obj_t *level = record_level_meter(parent, x, 72, color,
                                         color == CYBER_YELLOW_GREEN ? 234 : 216);
    if (x < 100)
        s_record_left_level = level;
    else
        s_record_right_level = level;
}

static void record_db_update_timer(lv_timer_t *timer)
{
    (void)timer;
    if (!s_record_page_active)
        return;

    if (s_record_cache_size != NULL)
    {
        const double megabytes = (double)record_audio_cache_bytes() /
                                 (1024.0 * 1024.0);
        char cache_text[24];
        snprintf(cache_text, sizeof(cache_text), "%.2f MB", megabytes);
        lv_label_set_text(s_record_cache_size, cache_text);
    }
    if (s_record_storage != NULL)
    {
        char storage_text[32];
        uint64_t total_bytes = 0U;
        uint64_t free_bytes = 0U;
        if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) == ESP_OK)
        {
            const double free_gb = (double)free_bytes /
                                   (1024.0 * 1024.0 * 1024.0);
            snprintf(storage_text, sizeof(storage_text), "SD // %.2f GB FREE", free_gb);
        }
        else
            snprintf(storage_text, sizeof(storage_text), "SD // -- FREE");
        lv_label_set_text(s_record_storage, storage_text);
    }

    if (s_record_elapsed != NULL)
    {
        const uint32_t milliseconds = record_audio_duration_ms();
        char elapsed[16];
        snprintf(elapsed, sizeof(elapsed), "%02lu:%02lu:%03lu",
                 (unsigned long)(milliseconds / 60000U),
                 (unsigned long)((milliseconds / 1000U) % 60U),
                 (unsigned long)(milliseconds % 1000U));
        lv_label_set_text(s_record_elapsed, elapsed);
    }
    if (s_record_record_text != NULL)
        lv_label_set_text(s_record_record_text,
                          record_audio_is_paused() ? "PAUSED" : record_audio_is_recording() ? "RECORDING"
                                                                                            : "RECORD");
    if (s_record_play_text != NULL)
        lv_label_set_text(s_record_play_text,
                          (music_audio_is_playing() || record_audio_is_playing()) ? "PLAYING" : "PLAY");
    record_waveform_update();
    if (s_record_left_db == NULL || s_record_right_db == NULL ||
        record_audio_is_recording())
        return;

    float left_db = -60.0F;
    float right_db = -60.0F;
    if (!record_audio_read_db(&left_db, &right_db))
        return;

    char left_text[20];
    char right_text[20];
    snprintf(left_text, sizeof(left_text), "%+.1f DB", left_db);
    snprintf(right_text, sizeof(right_text), "%+.1f DB", right_db);
    lv_label_set_text(s_record_left_db, left_text);
    lv_label_set_text(s_record_right_db, right_text);

    const float left_ratio = left_db <= -60.0F ? 0.05F : left_db >= 0.0F ? 1.0F
                                                                         : 0.05F + (left_db + 60.0F) / 60.0F * 0.95F;
    const float right_ratio = right_db <= -60.0F ? 0.05F : right_db >= 0.0F ? 1.0F
                                                                            : 0.05F + (right_db + 60.0F) / 60.0F * 0.95F;
    if (s_record_left_level != NULL)
        lv_obj_set_width(s_record_left_level, (lv_coord_t)(296.0F * left_ratio));
    if (s_record_right_level != NULL)
        lv_obj_set_width(s_record_right_level, (lv_coord_t)(296.0F * right_ratio));
}

static lv_obj_t *record_control_label(lv_obj_t *parent, const char *text,
                                      lv_coord_t x, lv_coord_t width,
                                      uint32_t color)
{
    lv_obj_t *label = page_ui_label(parent, text,
                                    &lv_font_ShareTechMono1_16, color);
    lv_obj_set_width(label, width < 108 ? 108 : width);
    lv_obj_set_height(label, 22);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(label, x, 76);
    return label;
}

static void record_volume_sync_text(void)
{
    if (s_record_volume_text == NULL)
        return;

    char volume_text[16];
    snprintf(volume_text, sizeof(volume_text), "VOL %u%%",
             (unsigned)music_audio_get_volume_percent());
    lv_label_set_text(s_record_volume_text, volume_text);
}

static void record_volume_down_clicked_cb(lv_event_t *event)
{
    (void)event;
    const int32_t volume = (int32_t)music_audio_get_volume_percent();
    music_audio_set_volume_percent(volume - 5);
    record_volume_sync_text();
}

static void record_volume_up_clicked_cb(lv_event_t *event)
{
    (void)event;
    const int32_t volume = (int32_t)music_audio_get_volume_percent();
    music_audio_set_volume_percent(volume + 5);
    record_volume_sync_text();
}

static void record_pause_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (!record_audio_is_recording())
        return;
    const bool paused = !record_audio_is_paused();
    if (record_audio_pause(paused))
    {
        lv_obj_t *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
        lv_obj_t *label = button == NULL ? NULL : lv_obj_get_child(button, 0);
        if (label != NULL)
            lv_label_set_text(label, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

static bool record_file_supported(const char *name)
{
    const char *extension = strrchr(name, '.');
    if (extension == NULL)
        return false;
    return strcasecmp(extension, ".wav") == 0 ||
           strcasecmp(extension, ".mp3") == 0 ||
           strcasecmp(extension, ".aac") == 0 ||
           strcasecmp(extension, ".flac") == 0 ||
           strcasecmp(extension, ".m4a") == 0 ||
           strcasecmp(extension, ".ogg") == 0;
}

static void record_scan_directory(void)
{
    s_record_list.count = 0U;
    memset(s_record_list.durations_ms, 0, sizeof(s_record_list.durations_ms));
    DIR *directory = opendir(RECORD_DIRECTORY);
    if (directory == NULL)
        return;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           s_record_list.count < RECORD_FILE_LIMIT)
    {
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.' ||
            !record_file_supported(entry->d_name))
            continue;
        const size_t name_length = strlen(entry->d_name);
        const size_t copy_length = name_length < RECORD_FILE_NAME_SIZE - 1U
                                       ? name_length
                                       : RECORD_FILE_NAME_SIZE - 1U;
        memcpy(s_record_list.names[s_record_list.count], entry->d_name, copy_length);
        s_record_list.names[s_record_list.count][copy_length] = '\0';
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", RECORD_DIRECTORY, entry->d_name);
        (void)record_audio_wav_duration(
            path, &s_record_list.durations_ms[s_record_list.count]);
        ++s_record_list.count;
    }
    closedir(directory);
    for (size_t index = 1U; index < s_record_list.count; ++index)
    {
        char name[RECORD_FILE_NAME_SIZE];
        const uint32_t duration_ms = s_record_list.durations_ms[index];
        snprintf(name, sizeof(name), "%s", s_record_list.names[index]);
        size_t insert = index;
        while (insert > 0U && strcasecmp(s_record_list.names[insert - 1U], name) > 0)
        {
            snprintf(s_record_list.names[insert], RECORD_FILE_NAME_SIZE, "%s",
                     s_record_list.names[insert - 1U]);
            s_record_list.durations_ms[insert] = s_record_list.durations_ms[insert - 1U];
            --insert;
        }
        snprintf(s_record_list.names[insert], RECORD_FILE_NAME_SIZE, "%s", name);
        s_record_list.durations_ms[insert] = duration_ms;
    }
}

static void record_list_update_cache(void)
{
    if (s_record_list.viewport == NULL)
        return;
    const lv_coord_t scroll_y = lv_obj_get_scroll_y(s_record_list.viewport);
    size_t first = scroll_y > 0 ? (size_t)scroll_y / 46U : 0U;
    if (first > s_record_list.count)
        first = s_record_list.count;

    for (size_t slot = 0U; slot < RECORD_ROW_CACHE; ++slot)
    {
        const size_t index = first + slot;
        if (index >= s_record_list.count)
        {
            lv_obj_add_flag(s_record_list.rows[slot], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_record_list.rows[slot], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(s_record_list.rows[slot], (lv_coord_t)(index * 46U));
        char number[12];
        snprintf(number, sizeof(number), "%02u", (unsigned)(index + 1U));
        lv_label_set_text(s_record_list.numbers[slot], number);
        lv_label_set_text(s_record_list.labels[slot], s_record_list.names[index]);
        char duration[16] = "--:--";
        if (s_record_list.durations_ms[index] > 0U)
            snprintf(duration, sizeof(duration), "%02lu:%02lu",
                     (unsigned long)(s_record_list.durations_ms[index] / 60000U),
                     (unsigned long)((s_record_list.durations_ms[index] / 1000U) % 60U));
        lv_label_set_text(s_record_list.durations[slot], duration);
    }
}

static void record_list_scroll_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCROLL)
        record_list_update_cache();
}

static bool record_list_row_path(lv_obj_t *row, char *path, size_t size)
{
    if (row == NULL || path == NULL || size == 0U)
        return false;
    const lv_coord_t row_y = lv_obj_get_y(row);
    if (row_y < 0 || row_y % 46 != 0)
        return false;
    const size_t index = (size_t)row_y / 46U;
    if (index >= s_record_list.count)
        return false;
    snprintf(path, size, "%s/%s", RECORD_DIRECTORY, s_record_list.names[index]);
    return true;
}

static void record_list_play_cb(lv_event_t *event)
{
    lv_obj_t *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_obj_t *row = button == NULL ? NULL : lv_obj_get_parent(button);
    char path[192];
    if (record_audio_is_recording() || !record_list_row_path(row, path, sizeof(path)))
        return;
    char current[192];
    if (music_audio_current_path(current, sizeof(current)) &&
        strcmp(current, path) == 0 &&
        (music_audio_is_playing() || music_audio_is_paused()))
    {
        music_audio_stop();
        return;
    }
    (void)music_audio_play(path);
}

static void record_list_delete_cb(lv_event_t *event)
{
    lv_obj_t *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_obj_t *row = button == NULL ? NULL : lv_obj_get_parent(button);
    char path[192];
    if (!record_list_row_path(row, path, sizeof(path)))
        return;

    char current[192];
    if (music_audio_current_path(current, sizeof(current)) && strcmp(current, path) == 0)
        music_audio_stop();
    if (remove(path) == 0)
    {
        record_scan_directory();
        record_list_update_cache();
    }
}

static void record_file_row_cache_create(lv_obj_t *parent, size_t slot)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, RECORD_CONTENT_WIDTH - 24, 42);
    lv_obj_set_pos(row, 12, (lv_coord_t)(slot * 46U));
    lv_obj_set_style_bg_color(row, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(CYBER_GRID), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *number = page_ui_label(row, "01",
                                     &lv_font_ShareTechMono1_16, CYBER_YELLOW_GREEN);
    lv_obj_set_pos(number, 12, 11);

    lv_obj_t *file_name = page_ui_label(row, "",
                                        &lv_font_ShareTechMono1_16, CYBER_TEXT);
    lv_obj_set_width(file_name, 330);
    lv_label_set_long_mode(file_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(file_name, 50, 11);

    lv_obj_t *file_duration = page_ui_label(row, "--:--",
                                            &lv_font_ShareTechMono1_16, CYBER_CYAN);
    lv_obj_set_width(file_duration, 64);
    lv_obj_set_style_text_align(file_duration, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(file_duration, 398, 11);

    lv_obj_t *play = page_ui_icon_button(row, LV_SYMBOL_PLAY, 38, 34,
                                         record_list_play_cb, NULL);
    lv_obj_set_pos(play, 510, 4);
    lv_obj_set_style_radius(play, 0, 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(play, 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(play, 0), lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_add_flag(play, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *trash = page_ui_icon_button(row, LV_SYMBOL_TRASH, 38, 34,
                                          record_list_delete_cb, NULL);
    lv_obj_set_pos(trash, 566, 4);
    lv_obj_set_style_radius(trash, 0, 0);
    lv_obj_set_style_bg_opa(trash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(trash, 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(trash, 0), lv_color_hex(CYBER_MAGENTA), 0);
    lv_obj_add_flag(trash, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_record_list.rows[slot] = row;
    s_record_list.numbers[slot] = number;
    s_record_list.labels[slot] = file_name;
    s_record_list.durations[slot] = file_duration;
}

static void record_play_latest_cb(lv_event_t *event)
{
    (void)event;
    if (record_audio_is_recording())
        return;
    if (record_audio_is_playing())
    {
        (void)record_audio_stop_cached_playback();
        if (s_record_play_text != NULL)
            lv_label_set_text(s_record_play_text, "PLAY");
        return;
    }
    if (record_audio_play_cached())
    {
        if (s_record_play_text != NULL)
            lv_label_set_text(s_record_play_text, "PLAYING");
    }
}

static void record_save_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (record_audio_save())
    {
        record_scan_directory();
        record_list_update_cache();
    }
}

static void record_record_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (record_audio_is_recording())
    {
        if (record_audio_stop())
        {
            if (s_record_button_label != NULL)
                lv_label_set_text(s_record_button_label, LV_SYMBOL_AUDIO);
            if (s_record_record_text != NULL)
                lv_label_set_text(s_record_record_text, "RECORD");
            if (s_record_pause_button_label != NULL)
                lv_label_set_text(s_record_pause_button_label, LV_SYMBOL_PAUSE);
            record_scan_directory();
            record_list_update_cache();
        }
        return;
    }
    if (record_audio_start())
    {
        if (s_record_button_label != NULL)
            lv_label_set_text(s_record_button_label, LV_SYMBOL_STOP);
        if (s_record_record_text != NULL)
            lv_label_set_text(s_record_record_text, "RECORDING");
        if (s_record_pause_button_label != NULL)
            lv_label_set_text(s_record_pause_button_label, LV_SYMBOL_PAUSE);
    }
}

static void record_page_create(Page *page)
{
    lv_obj_set_style_bg_color(page->root, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_pad_all(page->root, 0, 0);
    lv_obj_set_scrollbar_mode(page->root, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *wave = record_panel(page->root, RECORD_SIDE_MARGIN, 58,
                                  RECORD_CONTENT_WIDTH, 220);
    lv_obj_t *caption = page_ui_label(wave, "PCM INPUT // DUAL CHANNEL",
                                      &lv_font_ShareTechMono1_16, CYBER_MUTED);
    lv_obj_set_pos(caption, 18, 13);
    lv_obj_t *format = page_ui_label(wave, "48 KHZ  /  16 BIT  /  WAV",
                                     &lv_font_ShareTechMono1_16, CYBER_YELLOW_GREEN);
    lv_obj_set_width(format, 250);
    lv_obj_set_style_text_align(format, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(format, RECORD_CONTENT_WIDTH - 268, 13);

    record_microphone_header(wave, 18, "MIC 1 // LEFT", "-18.4 DB", CYBER_YELLOW_GREEN);
    record_microphone_header(wave, 350, "MIC 2 // RIGHT", "-21.7 DB", CYBER_CYAN);
    record_waveform_channel(wave, 174, 153, CYBER_YELLOW_GREEN, CYBER_CYAN,
                            CYBER_MAGENTA, 2U);
    page_ui_add_top_left_bottom_right_brackets(wave, RECORD_CONTENT_WIDTH, 220, CYBER_YELLOW_GREEN, 0);

    s_record_elapsed = page_ui_label(page->root, "00:00:00",
                                     &lv_font_ShareTechMono1_32, CYBER_TEXT);
    lv_obj_set_width(s_record_elapsed, 200);
    lv_obj_set_style_text_align(s_record_elapsed, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_record_elapsed, 260, 329);
    s_record_cache_size = page_ui_label(page->root, "0.00 MB",
                                        &lv_font_ShareTechMono1_16, CYBER_MUTED);
    lv_obj_set_width(s_record_cache_size, 110);
    lv_obj_set_pos(s_record_cache_size, RECORD_SIDE_MARGIN, 340);
    s_record_volume_text = page_ui_label(page->root, "VOL 0%",
                                         &lv_font_ShareTechMono1_16, CYBER_MUTED);
    lv_obj_set_pos(s_record_volume_text, RECORD_SIDE_MARGIN + 120, 340);
    record_volume_sync_text();
    s_record_storage = page_ui_label(page->root, "SD // -- FREE",
                                     &lv_font_ShareTechMono1_16, CYBER_CYAN);
    lv_obj_set_width(s_record_storage, 190);
    lv_obj_set_style_text_align(s_record_storage, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_record_storage, RECORD_SCREEN_WIDTH - RECORD_SIDE_MARGIN - 190, 340);

    lv_obj_t *controls = lv_obj_create(page->root);
    lv_obj_set_size(controls, 600, 102);
    lv_obj_set_pos(controls, 60, 374);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    record_gesture_passthrough(controls);

    record_icon_button(controls, LV_SYMBOL_MINUS, 20, CYBER_MUTED, false,
                       record_volume_down_clicked_cb);
    lv_obj_t *record_button = record_icon_button(
        controls, LV_SYMBOL_AUDIO, 114, CYBER_YELLOW_GREEN, true, record_record_clicked_cb);
    s_record_button_label = lv_obj_get_child(record_button, 0);
    lv_obj_t *pause_button = record_icon_button(
        controls, LV_SYMBOL_PAUSE, 220, CYBER_YELLOW_GREEN, false, record_pause_clicked_cb);
    s_record_pause_button_label = lv_obj_get_child(pause_button, 0);
    record_icon_button(controls, LV_SYMBOL_PLAY, 320, CYBER_MAGENTA, false,
                       record_play_latest_cb);
    record_icon_button(controls, LV_SYMBOL_SAVE, 420, CYBER_CYAN, false,
                       record_save_clicked_cb);
    record_icon_button(controls, LV_SYMBOL_PLUS, 520, CYBER_MUTED, false,
                       record_volume_up_clicked_cb);
    s_record_record_text = record_control_label(controls, "RECORD", 96, 108, CYBER_YELLOW_GREEN);
    record_control_label(controls, "PAUSE", 196, 108, CYBER_YELLOW_GREEN);
    s_record_play_text = record_control_label(controls, "PLAY", 296, 108, CYBER_MAGENTA);
    record_control_label(controls, "SAVE", 396, 108, CYBER_CYAN);

    lv_obj_t *list = record_panel(page->root, RECORD_SIDE_MARGIN,
                                  RECORD_SCREEN_HEIGHT - 28 - 188,
                                  RECORD_CONTENT_WIDTH, 188);
    lv_obj_t *list_title = page_ui_label(list, "Recordings",
                                         &lv_font_ShareTechMono1_20, CYBER_TEXT);
    lv_obj_set_pos(list_title, 12, 9);
    lv_obj_t *path = page_ui_label(list, LV_SYMBOL_SD_CARD "  /sdcard/record",
                                   &lv_font_ShareTechMono1_16, CYBER_MUTED);
    lv_obj_set_width(path, 250);
    lv_obj_set_style_text_align(path, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(path, RECORD_CONTENT_WIDTH - 262, 11);

    s_record_list.viewport = lv_obj_create(list);
    lv_obj_set_size(s_record_list.viewport, RECORD_CONTENT_WIDTH - 24, 140);
    lv_obj_set_pos(s_record_list.viewport, 12, 38);
    lv_obj_set_style_bg_opa(s_record_list.viewport, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_record_list.viewport, 0, 0);
    lv_obj_set_style_pad_all(s_record_list.viewport, 0, 0);
    lv_obj_set_scrollbar_mode(s_record_list.viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_record_list.viewport, LV_DIR_VER);
    lv_obj_add_flag(s_record_list.viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_record_list.viewport, record_list_scroll_cb, LV_EVENT_SCROLL, NULL);
    record_scan_directory();
    for (size_t slot = 0U; slot < RECORD_ROW_CACHE; ++slot)
        record_file_row_cache_create(s_record_list.viewport, slot);
    record_list_update_cache();

    s_record_db_timer = lv_timer_create(record_db_update_timer, RECORD_DB_UPDATE_MS, NULL);
}

static void record_page_enter(Page *page)
{
    (void)page;
    s_record_page_active = record_audio_enable_capture();
    if (!s_record_page_active)
        return;
    record_scan_directory();
    record_list_update_cache();
    record_db_update_timer(NULL);
    record_volume_sync_text();
}

static void record_page_leave(Page *page)
{
    (void)page;
    s_record_page_active = false;
    /* Page switching only calls on_leave; release this page's UI tree now. */
    (void)ui_page_destroy(PAGE_RECORD);
}

static void record_page_destroy(Page *page)
{
    (void)page;
    s_record_page_active = false;

    /* Stop any recorder/cache playback before the page-owned UI disappears. */
    record_audio_clear_cache();
    (void)record_audio_disable_capture();

    if (s_record_db_timer != NULL)
    {
        lv_timer_del(s_record_db_timer);
        s_record_db_timer = NULL;
    }

    /* All child widgets are owned by page->root and deleted by the manager. */
    s_record_left_db = NULL;
    s_record_right_db = NULL;
    s_record_left_level = NULL;
    s_record_right_level = NULL;
    s_record_button_label = NULL;
    s_record_elapsed = NULL;
    s_record_cache_size = NULL;
    s_record_volume_text = NULL;
    s_record_storage = NULL;
    s_record_record_text = NULL;
    s_record_play_text = NULL;
    s_record_pause_button_label = NULL;
    memset(s_record_wave_bars, 0, sizeof(s_record_wave_bars));
    memset(s_record_wave_heights, 0, sizeof(s_record_wave_heights));
    memset(&s_record_list, 0, sizeof(s_record_list));
}

extern "C" void record_page_register(void)
{
    Page page{};

    page.id = PAGE_RECORD;

    page.name = "record";

    page.on_create = record_page_create;

    // page.timer = record_db_update_timer;
    // page.timer_ms = RECORD_DB_UPDATE_MS;

    page.on_destroy = record_page_destroy;

    page.on_enter = record_page_enter;

    page.on_leave = record_page_leave;

    page.on_gesture = page_ui_back_gesture;
    ui_page_register(&page);
}
