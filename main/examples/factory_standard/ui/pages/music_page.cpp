#include "page_common.h"
#include "libs/tjpgd/tjpgd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>

#include <app/services/music_audio.h>
#include <app/services/service_context.hpp>
#include <bsp/t_panel_p4_standard.hpp>

#define MUSIC_DIRECTORY "/sdcard/music"
#define MUSIC_SCREEN_WIDTH 720
#define MUSIC_SIDE_MARGIN 28
#define MUSIC_MAX_TRACKS 32
#define MUSIC_PLAYLIST_ROW_CACHE 10
#define MUSIC_PATH_SIZE 192
#define MUSIC_NAME_SIZE 96
#define MUSIC_LYRICS_SIZE 1024
#define MUSIC_MAX_LYRICS_LINES 32

#define MUSIC_COVER_X 28
#define MUSIC_COVER_Y 90
#define MUSIC_COVER_SIZE 200
#define MUSIC_SPECTRUM_X MUSIC_COVER_X
#define MUSIC_SPECTRUM_Y 330
#define MUSIC_SPECTRUM_WIDTH 200
#define MUSIC_SPECTRUM_HEIGHT 150
#define MUSIC_SPECTRUM_BARS 16
#define MUSIC_SPECTRUM_SAMPLES 64
#define MUSIC_INFO_X 270
#define MUSIC_INFO_Y 68
#define MUSIC_INFO_WIDTH (MUSIC_SCREEN_WIDTH - MUSIC_SIDE_MARGIN - MUSIC_INFO_X)
#define MUSIC_LYRICS_Y 142
#define MUSIC_PROGRESS_Y 358
#define MUSIC_CONTROLS_Y 400
#define MUSIC_VOLUME_Y 524
#define MUSIC_VOLUME_HEIGHT 15
#define MUSIC_ENERGY_Y 688
#define MUSIC_ENERGY_HEIGHT 3
#define MUSIC_ENERGY_TRACK_WIDTH (MUSIC_SCREEN_WIDTH - MUSIC_SIDE_MARGIN * 2)
#define MUSIC_ENERGY_PULSE_COUNT 3
#define MUSIC_ENERGY_CORE_SIZE 6
#define MUSIC_LIST_X 270
#define MUSIC_LIST_Y 54
#define MUSIC_LIST_WIDTH (MUSIC_SCREEN_WIDTH - MUSIC_SIDE_MARGIN - MUSIC_LIST_X)
#define MUSIC_LIST_HEIGHT 612

typedef enum
{
    MUSIC_ORDER_SEQUENCE = 0,
    MUSIC_ORDER_SHUFFLE,
    MUSIC_ORDER_REPEAT,
} music_order_mode_t;

typedef struct
{
    char path[MUSIC_PATH_SIZE];
    char name[MUSIC_NAME_SIZE];
    char title[MUSIC_NAME_SIZE];
    char artist[MUSIC_NAME_SIZE];
    char album[MUSIC_NAME_SIZE];
    uint32_t duration_ms;
} music_track_t;

typedef struct
{
    lv_obj_t *root;
    lv_obj_t *cover;
    lv_obj_t *cover_image;
    lv_obj_t *cover_placeholder;
    uint8_t *cover_data;
    size_t cover_size;
    lv_draw_buf_t *cover_buf;
    lv_image_dsc_t cover_dsc;
    lv_obj_t *title;
    lv_obj_t *artist;
    lv_obj_t *album;
    lv_obj_t *spectrum;
    lv_obj_t *spectrum_bars[MUSIC_SPECTRUM_BARS];
    uint8_t spectrum_levels[MUSIC_SPECTRUM_BARS];
    float spectrum_display[MUSIC_SPECTRUM_BARS];
    uint32_t spectrum_phase;
    lv_obj_t *lyric_lines[5];
    lv_obj_t *progress;
    lv_obj_t *elapsed;
    lv_obj_t *duration;
    lv_obj_t *play_label;
    bool play_symbol_initialized;
    bool play_symbol_is_pause;
    lv_obj_t *volume;
    lv_obj_t *volume_text;
    lv_obj_t *energy_track;
    lv_obj_t *energy_pulses[MUSIC_ENERGY_PULSE_COUNT];
    lv_obj_t *energy_core;
    float energy_level;
    uint32_t energy_last_tick;
    lv_obj_t *playlist;
    lv_obj_t *playlist_list;
    lv_obj_t *playlist_spacer;
    lv_obj_t *playlist_rows[MUSIC_PLAYLIST_ROW_CACHE];
    lv_obj_t *order_label;
    lv_obj_t *audio_params_primary;
    lv_obj_t *audio_params_secondary;
    size_t displayed_track_index;
    bool displayed_track_valid;
    lv_timer_t *timer;
    music_track_t *tracks;
    size_t track_count;
    size_t current_index;
    bool playlist_scanned;
    music_order_mode_t order_mode;
    char title_text[96];
    char artist_text[96];
    char album_text[96];
    char lyrics_text[MUSIC_LYRICS_SIZE];
    char lyric_text_lines[MUSIC_MAX_LYRICS_LINES][MUSIC_NAME_SIZE];
    uint32_t lyric_line_times[MUSIC_MAX_LYRICS_LINES];
    size_t lyric_line_count;
} music_page_ui_t;

static music_page_ui_t s_music_ui;
static portMUX_TYPE s_music_playlist_lock = portMUX_INITIALIZER_UNLOCKED;
static constexpr char kMusicPageTag[] = "factory_music_diag";
static constexpr const char *kMusicComponentTags[] = {
    "ESP_GMF_FILE",
    "ESP_GMF_ASMP_DEC",
    "AUD_SIMP_PLAYER",
};
static uint32_t s_diag_last_ms;
static uint32_t s_diag_last_position_ms;
static bsp::t_panel_p4_standard::MusicStatistics s_diag_last_music_stats{};
static bsp::t_panel_p4_standard::AudioStatistics s_diag_last_audio_stats{};
static float s_spectrum_sin[MUSIC_SPECTRUM_SAMPLES];
static float s_spectrum_cos[MUSIC_SPECTRUM_SAMPLES];
static bool s_spectrum_table_initialized;

static void music_spectrum_init_table(void)
{
    if (s_spectrum_table_initialized)
        return;

    for (size_t index = 0U; index < MUSIC_SPECTRUM_SAMPLES; ++index)
    {
        const float angle = 2.0F * 3.14159265F * (float)index /
                            (float)MUSIC_SPECTRUM_SAMPLES;
        s_spectrum_sin[index] = sinf(angle);
        s_spectrum_cos[index] = cosf(angle);
    }
    s_spectrum_table_initialized = true;
}

static const char *const s_fallback_lyrics[] = {
    "NO MEDIA LINK // INSERT SD",
    "LYRIC STREAM // STANDBY",
    "SELECT TRACK TO INITIALIZE",
    "",
    "",
};

static lv_obj_t *music_button(
    lv_obj_t *parent,
    const char *symbol,
    lv_coord_t width,
    lv_coord_t height,
    lv_event_cb_t callback,
    void *user_data)
{
    lv_obj_t *button = page_ui_icon_button(parent, symbol, width, height, callback, user_data);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(CYBER_YELLOW), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_shadow_spread(button, 0, 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, 0);
    lv_obj_set_style_text_color(button, lv_color_hex(CYBER_TEXT), 0);
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label != NULL)
    {
        lv_obj_set_style_text_color(label, lv_color_hex(CYBER_TEXT), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    }
    return button;
}

static void music_gesture_passthrough(lv_obj_t *object)
{
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(object, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void music_spectrum_create(lv_obj_t *parent)
{
    s_music_ui.spectrum = lv_obj_create(parent);
    lv_obj_set_size(s_music_ui.spectrum, MUSIC_SPECTRUM_WIDTH, MUSIC_SPECTRUM_HEIGHT);
    lv_obj_set_pos(s_music_ui.spectrum, MUSIC_SPECTRUM_X, MUSIC_SPECTRUM_Y);
    lv_obj_set_style_bg_opa(s_music_ui.spectrum, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_music_ui.spectrum, 0, 0);
    lv_obj_set_style_radius(s_music_ui.spectrum, 0, 0);
    lv_obj_set_style_pad_all(s_music_ui.spectrum, 0, 0);
    music_gesture_passthrough(s_music_ui.spectrum);

    for (size_t index = 0U; index < MUSIC_SPECTRUM_BARS; ++index)
    {
        const uint32_t ratio = index * 255U / (MUSIC_SPECTRUM_BARS - 1U);
        const uint32_t start = CYBER_CYAN;
        const uint32_t end = CYBER_YELLOW_GREEN;
        const uint32_t red = (((start >> 16U) * (255U - ratio)) + ((end >> 16U) * ratio)) / 255U;
        const uint32_t green = ((((start >> 8U) & 0xffU) * (255U - ratio)) + (((end >> 8U) & 0xffU) * ratio)) / 255U;
        const uint32_t blue = (((start & 0xffU) * (255U - ratio)) + ((end & 0xffU) * ratio)) / 255U;
        const uint32_t color = (red << 16U) | (green << 8U) | blue;
        lv_obj_t *bar = lv_obj_create(s_music_ui.spectrum);
        s_music_ui.spectrum_bars[index] = bar;
        s_music_ui.spectrum_levels[index] = 4U;
        lv_obj_set_size(bar, 8, 4);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_shadow_width(bar, 6, 0);
        lv_obj_set_style_shadow_spread(bar, 1, 0);
        lv_obj_set_style_shadow_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_shadow_opa(bar, LV_OPA_30, 0);
        music_gesture_passthrough(bar);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, 6 + (lv_coord_t)(index * 12U), 0);
    }
}

static void music_energy_create(lv_obj_t *parent)
{
    s_music_ui.energy_track = lv_obj_create(parent);
    lv_obj_set_size(s_music_ui.energy_track,
                    MUSIC_ENERGY_TRACK_WIDTH, MUSIC_ENERGY_HEIGHT);
    lv_obj_set_pos(s_music_ui.energy_track, MUSIC_SIDE_MARGIN, MUSIC_ENERGY_Y);
    lv_obj_set_style_bg_color(s_music_ui.energy_track, lv_color_hex(CYBER_GRID), 0);
    lv_obj_set_style_bg_opa(s_music_ui.energy_track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_music_ui.energy_track, 0, 0);
    lv_obj_set_style_radius(s_music_ui.energy_track, 0, 0);
    lv_obj_set_style_pad_all(s_music_ui.energy_track, 0, 0);
    music_gesture_passthrough(s_music_ui.energy_track);

    static const lv_coord_t pulse_widths[MUSIC_ENERGY_PULSE_COUNT] = {96, 240, 440};
    static const lv_coord_t pulse_heights[MUSIC_ENERGY_PULSE_COUNT] = {4, 2, 1};
    static const uint32_t pulse_colors[MUSIC_ENERGY_PULSE_COUNT] = {
        CYBER_YELLOW_GREEN, CYBER_CYAN, CYBER_CYAN};
    for (size_t index = 0U; index < MUSIC_ENERGY_PULSE_COUNT; ++index)
    {
        s_music_ui.energy_pulses[index] = lv_obj_create(parent);
        lv_obj_set_size(s_music_ui.energy_pulses[index],
                        pulse_widths[index], pulse_heights[index]);
        lv_obj_set_pos(s_music_ui.energy_pulses[index],
                       (MUSIC_SCREEN_WIDTH - pulse_widths[index]) / 2,
                       MUSIC_ENERGY_Y - pulse_heights[index] / 2);
        lv_obj_set_style_bg_color(s_music_ui.energy_pulses[index],
                                  lv_color_hex(pulse_colors[index]), 0);
        lv_obj_set_style_bg_opa(s_music_ui.energy_pulses[index], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_music_ui.energy_pulses[index], 0, 0);
        lv_obj_set_style_radius(s_music_ui.energy_pulses[index], 1, 0);
        lv_obj_set_style_pad_all(s_music_ui.energy_pulses[index], 0, 0);
        lv_obj_set_style_shadow_color(s_music_ui.energy_pulses[index],
                                      lv_color_hex(pulse_colors[index]), 0);
        lv_obj_set_style_shadow_width(s_music_ui.energy_pulses[index],
                                      index == 0U ? 12 : 8, 0);
        lv_obj_set_style_shadow_spread(s_music_ui.energy_pulses[index], 1, 0);
        lv_obj_set_style_shadow_opa(s_music_ui.energy_pulses[index], LV_OPA_TRANSP, 0);
        music_gesture_passthrough(s_music_ui.energy_pulses[index]);
    }

    s_music_ui.energy_core = lv_obj_create(parent);
    lv_obj_set_size(s_music_ui.energy_core, MUSIC_ENERGY_CORE_SIZE,
                    MUSIC_ENERGY_CORE_SIZE);
    lv_obj_set_pos(s_music_ui.energy_core,
                   (MUSIC_SCREEN_WIDTH - MUSIC_ENERGY_CORE_SIZE) / 2,
                   MUSIC_ENERGY_Y - MUSIC_ENERGY_CORE_SIZE / 2);
    lv_obj_set_style_bg_color(s_music_ui.energy_core,
                              lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_bg_opa(s_music_ui.energy_core, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_music_ui.energy_core, 0, 0);
    lv_obj_set_style_radius(s_music_ui.energy_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(s_music_ui.energy_core, 0, 0);
    lv_obj_set_style_shadow_color(s_music_ui.energy_core,
                                  lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_shadow_width(s_music_ui.energy_core, 14, 0);
    lv_obj_set_style_shadow_spread(s_music_ui.energy_core, 2, 0);
    lv_obj_set_style_shadow_opa(s_music_ui.energy_core, LV_OPA_TRANSP, 0);
    music_gesture_passthrough(s_music_ui.energy_core);

    s_music_ui.energy_level = 0.0F;
    s_music_ui.energy_last_tick = lv_tick_get();
}

static void music_energy_update(bool active)
{
    if (s_music_ui.energy_track == NULL || s_music_ui.energy_core == NULL ||
        s_music_ui.energy_pulses[0] == NULL ||
        s_music_ui.energy_pulses[1] == NULL ||
        s_music_ui.energy_pulses[2] == NULL)
        return;

    const uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_music_ui.energy_last_tick;
    s_music_ui.energy_last_tick = now;
    if (elapsed > 100U)
        elapsed = 100U;

    const float target_level = active
                                   ? (float)music_audio_level_percent() / 100.0F
                                   : 0.0F;
    const float response = active ? 0.36F : 0.14F;
    s_music_ui.energy_level +=
        (target_level - s_music_ui.energy_level) * response;
    const float elapsed_scale = elapsed > 0U ? (float)elapsed / 33.0F : 1.0F;
    const float pulse = active
                            ? s_music_ui.energy_level
                            : s_music_ui.energy_level * (1.0F - 0.08F * elapsed_scale);
    static const lv_coord_t base_widths[MUSIC_ENERGY_PULSE_COUNT] = {96, 240, 440};
    static const lv_coord_t spread_widths[MUSIC_ENERGY_PULSE_COUNT] = {88, 180, 180};
    static const lv_opa_t base_opacity[MUSIC_ENERGY_PULSE_COUNT] = {180, 80, 35};
    for (size_t index = 0U; index < MUSIC_ENERGY_PULSE_COUNT; ++index)
    {
        const lv_coord_t width = base_widths[index] +
                                 (lv_coord_t)(spread_widths[index] * pulse);
        const lv_opa_t opacity = active
                                     ? (lv_opa_t)(base_opacity[index] +
                                                  (255U - base_opacity[index]) * pulse)
                                     : (lv_opa_t)(base_opacity[index] * pulse);
        const lv_coord_t height = index == 0U
                                      ? (lv_coord_t)(MUSIC_ENERGY_HEIGHT + 1U + pulse * 3.0F)
                                      : MUSIC_ENERGY_HEIGHT;
        lv_obj_set_width(s_music_ui.energy_pulses[index], width);
        lv_obj_set_height(s_music_ui.energy_pulses[index], height);
        lv_obj_set_x(s_music_ui.energy_pulses[index],
                     (MUSIC_SCREEN_WIDTH - width) / 2);
        lv_obj_set_y(s_music_ui.energy_pulses[index],
                     MUSIC_ENERGY_Y - height / 2);
        lv_obj_set_style_bg_opa(s_music_ui.energy_pulses[index], opacity, 0);
        lv_obj_set_style_shadow_opa(s_music_ui.energy_pulses[index],
                                    (lv_opa_t)(opacity / 2U), 0);
    }

    const lv_coord_t core_size = (lv_coord_t)(MUSIC_ENERGY_CORE_SIZE + pulse * 8.0F);
    const lv_opa_t core_opa = active ? (lv_opa_t)(140.0F + pulse * 115.0F)
                                     : (lv_opa_t)(pulse * 70.0F);
    lv_obj_set_size(s_music_ui.energy_core, core_size, core_size);
    lv_obj_set_pos(s_music_ui.energy_core,
                   (MUSIC_SCREEN_WIDTH - core_size) / 2,
                   MUSIC_ENERGY_Y - core_size / 2);
    lv_obj_set_style_bg_opa(s_music_ui.energy_core, core_opa, 0);
    lv_obj_set_style_shadow_opa(s_music_ui.energy_core,
                                (lv_opa_t)(core_opa / 2U), 0);
}

static bool music_buffers_allocate(void)
{
    if (s_music_ui.tracks != NULL)
        return true;
    s_music_ui.tracks = static_cast<music_track_t *>(heap_caps_calloc(MUSIC_MAX_TRACKS, sizeof(*s_music_ui.tracks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return s_music_ui.tracks != NULL;
}

static size_t music_next_index(bool previous);

// This callback is called by the p4_music task, so it must not touch LVGL.
// The playlist metadata is kept alive after the page tree is destroyed.
static bool music_next_track_for_player(char *path, size_t path_size, void *)
{
    if (path == NULL || path_size == 0U)
        return false;

    // The music page owns this callback, but the shared BSP player is also
    // used by the record page.  Never let a recording finish and fall into
    // the music playlist.  The current path is read before selecting the
    // next item, so an automatic advance is only valid for music tracks.
    char current_path[MUSIC_PATH_SIZE]{};
    const size_t music_directory_length = sizeof(MUSIC_DIRECTORY) - 1U;
    if (!music_audio_current_path(current_path, sizeof(current_path)) ||
        strncmp(current_path, MUSIC_DIRECTORY, music_directory_length) != 0 ||
        current_path[music_directory_length] != '/')
        return false;

    portENTER_CRITICAL(&s_music_playlist_lock);
    if (s_music_ui.tracks == NULL || s_music_ui.track_count == 0U)
    {
        portEXIT_CRITICAL(&s_music_playlist_lock);
        return false;
    }
    const size_t index = music_next_index(false);
    if (index >= s_music_ui.track_count)
    {
        portEXIT_CRITICAL(&s_music_playlist_lock);
        return false;
    }
    s_music_ui.current_index = index;
    snprintf(path, path_size, "%s", s_music_ui.tracks[index].path);
    portEXIT_CRITICAL(&s_music_playlist_lock);
    return path[0] != '\0';
}

static void music_format_time(uint32_t milliseconds, char *buffer, size_t size)
{
    const uint32_t seconds = milliseconds / 1000U;
    snprintf(buffer, size, "%02lu:%02lu",
             (unsigned long)(seconds / 60U),
             (unsigned long)(seconds % 60U));
}

static void music_set_play_symbol(void)
{
    if (s_music_ui.play_label == NULL)
        return;
    const bool show_pause = music_audio_is_playing() && !music_audio_is_paused();
    if (s_music_ui.play_symbol_initialized &&
        s_music_ui.play_symbol_is_pause == show_pause)
        return;
    lv_label_set_text(s_music_ui.play_label,
                      show_pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_size(s_music_ui.play_label, 32, 32);
    lv_obj_set_style_text_align(s_music_ui.play_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_music_ui.play_label);
    s_music_ui.play_symbol_is_pause = show_pause;
    s_music_ui.play_symbol_initialized = true;
}

static const char *music_current_path(void)
{
    return s_music_ui.track_count > 0U
               ? s_music_ui.tracks[s_music_ui.current_index].path
               : NULL;
}

static void music_scan_directory(void)
{
    s_music_ui.track_count = 0U;
    s_music_ui.current_index = 0U;
    s_music_ui.playlist_scanned = false;
    if (s_music_ui.tracks == NULL)
        return;
    DIR *directory = opendir(MUSIC_DIRECTORY);
    if (directory == NULL)
        return;

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL &&
           s_music_ui.track_count < MUSIC_MAX_TRACKS)
    {
        if (entry->d_type == DT_DIR || entry->d_name[0] == '.')
            continue;
        const char *extension = strrchr(entry->d_name, '.');
        if (extension == NULL || strcasecmp(extension, ".mp3") != 0)
            continue;

        music_track_t *track = &s_music_ui.tracks[s_music_ui.track_count];
        const size_t directory_length = strlen(MUSIC_DIRECTORY);
        const size_t entry_length = strlen(entry->d_name);
        if (directory_length + 1U + entry_length + 1U > sizeof(track->path))
            continue;
        memcpy(track->path, MUSIC_DIRECTORY, directory_length);
        track->path[directory_length] = '/';
        memcpy(track->path + directory_length + 1U,
               entry->d_name, entry_length + 1U);
        const size_t name_length = entry_length < sizeof(track->name) - 1U
                                       ? entry_length
                                       : sizeof(track->name) - 1U;
        memcpy(track->name, entry->d_name, name_length);
        track->name[name_length] = '\0';
        char *dot = strrchr(track->name, '.');
        if (dot != NULL)
            *dot = '\0';
        snprintf(track->title, sizeof(track->title), "%s", track->name);
        snprintf(track->artist, sizeof(track->artist), "%s", "Unknown artist");
        snprintf(track->album, sizeof(track->album), "%s", "Unknown album");
        char metadata_title[MUSIC_NAME_SIZE];
        char metadata_artist[MUSIC_NAME_SIZE];
        char metadata_album[MUSIC_NAME_SIZE];
        if (music_audio_read_metadata(track->path,
                                      metadata_title, sizeof(metadata_title),
                                      metadata_artist, sizeof(metadata_artist),
                                      metadata_album, sizeof(metadata_album)))
        {
            if (metadata_title[0] != '\0')
                snprintf(track->title, sizeof(track->title), "%s", metadata_title);
            if (metadata_artist[0] != '\0')
                snprintf(track->artist, sizeof(track->artist), "%s", metadata_artist);
            if (metadata_album[0] != '\0')
                snprintf(track->album, sizeof(track->album), "%s", metadata_album);
        }
        ++s_music_ui.track_count;
    }
    closedir(directory);
    s_music_ui.playlist_scanned = true;

    for (size_t index = 1; index < s_music_ui.track_count; ++index)
    {
        music_track_t track = s_music_ui.tracks[index];
        size_t insert = index;
        while (insert > 0U &&
               strcasecmp(s_music_ui.tracks[insert - 1U].title, track.title) > 0)
        {
            s_music_ui.tracks[insert] = s_music_ui.tracks[insert - 1U];
            --insert;
        }
        s_music_ui.tracks[insert] = track;
    }

    /* Keep page entry fast: only probe the ten rows that make up the initial
     * playlist cache. Other rows are probed on demand when they are selected
     * or become visible. */
    const size_t initial_duration_count = s_music_ui.track_count < MUSIC_PLAYLIST_ROW_CACHE
                                              ? s_music_ui.track_count
                                              : MUSIC_PLAYLIST_ROW_CACHE;
    for (size_t index = 0U; index < initial_duration_count; ++index)
        (void)music_audio_probe_duration(s_music_ui.tracks[index].path,
                                          &s_music_ui.tracks[index].duration_ms);
}

static void music_set_fallback_metadata(void)
{
    strcpy(s_music_ui.title_text, "No MP3 selected");
    strcpy(s_music_ui.artist_text, "Unknown artist");
    strcpy(s_music_ui.album_text, "Unknown album");
    if (s_music_ui.track_count > 0U)
    {
        const music_track_t *track = &s_music_ui.tracks[s_music_ui.current_index];
        strncpy(s_music_ui.title_text,
                track->title,
                sizeof(s_music_ui.title_text) - 1U);
        s_music_ui.title_text[sizeof(s_music_ui.title_text) - 1U] = '\0';
        snprintf(s_music_ui.artist_text, sizeof(s_music_ui.artist_text), "%s", track->artist);
        snprintf(s_music_ui.album_text, sizeof(s_music_ui.album_text), "%s", track->album);
    }
}

static void music_ensure_track_duration(size_t index)
{
    if (index >= s_music_ui.track_count || s_music_ui.tracks[index].duration_ms != 0U)
        return;
    (void)music_audio_probe_duration(s_music_ui.tracks[index].path,
                                      &s_music_ui.tracks[index].duration_ms);
}

[[maybe_unused]] static bool music_cover_dimensions(const uint8_t *data, size_t size,
                                   uint32_t *width, uint32_t *height)
{
    if (data == NULL || width == NULL || height == NULL)
        return false;
    if (size >= 24U && memcmp(data, "\x89PNG\r\n\x1a\n", 8U) == 0)
    {
        *width = (uint32_t(data[16]) << 24U) | (uint32_t(data[17]) << 16U) |
                 (uint32_t(data[18]) << 8U) | data[19];
        *height = (uint32_t(data[20]) << 24U) | (uint32_t(data[21]) << 16U) |
                  (uint32_t(data[22]) << 8U) | data[23];
        return *width != 0U && *height != 0U;
    }
    if (size < 4U || data[0] != 0xffU || data[1] != 0xd8U)
        return false;
    size_t offset = 2U;
    while (offset + 4U <= size)
    {
        if (data[offset] != 0xffU)
        {
            ++offset;
            continue;
        }
        while (offset < size && data[offset] == 0xffU)
            ++offset;
        if (offset >= size)
            break;
        const uint8_t marker = data[offset++];
        if (marker == 0xd8U || marker == 0xd9U)
            continue;
        if (offset + 2U > size)
            break;
        const size_t segment_size = (size_t(data[offset]) << 8U) | data[offset + 1U];
        if (segment_size < 2U || offset + segment_size > size)
            break;
        const bool sof = (marker >= 0xc0U && marker <= 0xc3U) ||
                         (marker >= 0xc5U && marker <= 0xc7U) ||
                         (marker >= 0xc9U && marker <= 0xcBU) ||
                         (marker >= 0xcDU && marker <= 0xcFU);
        if (sof && segment_size >= 7U)
        {
            *height = (uint32_t(data[offset + 3U]) << 8U) | data[offset + 4U];
            *width = (uint32_t(data[offset + 5U]) << 8U) | data[offset + 6U];
            return *width != 0U && *height != 0U;
        }
        offset += segment_size;
    }
    return false;
}

static void music_cover_release(void)
{
    /* Detach the descriptor before returning its encoded buffer.  The image
     * decoder may retain the descriptor address while the object is reset. */
    s_music_ui.cover_dsc.data = NULL;
    s_music_ui.cover_dsc.data_size = 0U;
    if (s_music_ui.cover_image != NULL)
        lv_image_set_src(s_music_ui.cover_image, NULL);
    if (s_music_ui.cover_buf != NULL)
        lv_draw_buf_destroy(s_music_ui.cover_buf);
    s_music_ui.cover_buf = NULL;
    /* The LVGL decoder can retain the source descriptor until its cache is
     * flushed. Do not free the encoded APIC buffer from the page callback;
     * releasing it here can pass a stale source address to heap_caps_free. */
    s_music_ui.cover_data = NULL;
    s_music_ui.cover_size = 0U;
    memset(&s_music_ui.cover_dsc, 0, sizeof(s_music_ui.cover_dsc));
}

typedef struct
{
    const uint8_t *data;
    size_t size;
    size_t offset;
    uint16_t *pixels;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t target_x;
    uint32_t target_y;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t target_stride_pixels;
} music_jpeg_context_t;

static size_t music_jpeg_input(JDEC *decoder, uint8_t *buffer, size_t size)
{
    music_jpeg_context_t *context = static_cast<music_jpeg_context_t *>(decoder->device);
    if (context == NULL || context->offset > context->size)
        return 0U;
    if (buffer == NULL)
    {
        const size_t next = context->offset + size;
        if (next > context->size)
            return 0U;
        context->offset = next;
        return size;
    }
    const size_t available = context->size - context->offset;
    const size_t count = size < available ? size : available;
    memcpy(buffer, context->data + context->offset, count);
    context->offset += count;
    return count;
}

static int music_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rect)
{
    music_jpeg_context_t *context = static_cast<music_jpeg_context_t *>(decoder->device);
    if (context == NULL || bitmap == NULL || rect == NULL)
        return 0;
    const uint8_t *source = static_cast<const uint8_t *>(bitmap);
    const uint32_t block_width = (uint32_t)rect->right - rect->left + 1U;
    for (uint32_t y = (uint32_t)rect->top; y <= (uint32_t)rect->bottom; ++y)
    {
        const uint32_t target_y = context->target_y +
                                  (y * context->target_height) / context->source_height;
        if (target_y >= MUSIC_COVER_SIZE)
            continue;
        for (uint32_t x = (uint32_t)rect->left; x <= (uint32_t)rect->right; ++x)
        {
            const uint32_t target_x = context->target_x +
                                      (x * context->target_width) / context->source_width;
            if (target_x >= MUSIC_COVER_SIZE)
                continue;
            const uint32_t source_x = x - rect->left;
            const uint32_t source_y = y - rect->top;
            const uint8_t *pixel = source +
                                   (source_y * block_width + source_x) * 3U;
            /* TJpgDec emits BGR888. */
            context->pixels[target_y * context->target_stride_pixels + target_x] =
                (uint16_t)(((pixel[2] >> 3U) << 11U) |
                           ((pixel[1] >> 2U) << 5U) |
                           (pixel[0] >> 3U));
        }
    }
    return 1;
}

static lv_draw_buf_t *music_decode_jpeg(const uint8_t *data, size_t size)
{
    if (data == NULL || size < 4U || data[0] != 0xffU || data[1] != 0xd8U)
        return NULL;
    lv_draw_buf_t *draw_buf = lv_draw_buf_create(
        MUSIC_COVER_SIZE, MUSIC_COVER_SIZE, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (draw_buf == NULL)
        return NULL;
    memset(draw_buf->data, 0, draw_buf->data_size);
    music_jpeg_context_t context{};
    context.data = data;
    context.size = size;
    context.pixels = reinterpret_cast<uint16_t *>(draw_buf->data);
    context.target_stride_pixels = draw_buf->header.stride / sizeof(uint16_t);
    uint8_t *work = static_cast<uint8_t *>(malloc(4096U));
    if (work == NULL)
    {
        lv_draw_buf_destroy(draw_buf);
        return NULL;
    }
    JDEC decoder{};
    const JRESULT prepared = jd_prepare(&decoder, music_jpeg_input, work, 4096U, &context);
    if (prepared != JDR_OK || decoder.width == 0U || decoder.height == 0U)
    {
        free(work);
        lv_draw_buf_destroy(draw_buf);
        return NULL;
    }
    context.source_width = decoder.width;
    context.source_height = decoder.height;
    const uint32_t scale_x = MUSIC_COVER_SIZE * decoder.height;
    const uint32_t scale_y = MUSIC_COVER_SIZE * decoder.width;
    if (scale_x < scale_y)
    {
        context.target_width = MUSIC_COVER_SIZE;
        context.target_height = scale_x / decoder.width;
    }
    else
    {
        context.target_height = MUSIC_COVER_SIZE;
        context.target_width = scale_y / decoder.height;
    }
    if (context.target_width == 0U)
        context.target_width = 1U;
    if (context.target_height == 0U)
        context.target_height = 1U;
    context.target_x = (MUSIC_COVER_SIZE - context.target_width) / 2U;
    context.target_y = (MUSIC_COVER_SIZE - context.target_height) / 2U;
    const JRESULT decoded = jd_decomp(&decoder, music_jpeg_output, 0U);
    free(work);
    if (decoded != JDR_OK)
    {
        lv_draw_buf_destroy(draw_buf);
        return NULL;
    }
    return draw_buf;
}

[[maybe_unused]] static void music_cover_fit(uint32_t width, uint32_t height)
{
    if (s_music_ui.cover_image == NULL || width == 0U || height == 0U)
        return;
    const uint32_t scale_x = (MUSIC_COVER_SIZE * 256U) / width;
    const uint32_t scale_y = (MUSIC_COVER_SIZE * 256U) / height;
    const uint32_t scale = scale_x < scale_y ? scale_x : scale_y;
    lv_image_set_scale(s_music_ui.cover_image, scale == 0U ? 1U : scale);
    lv_obj_center(s_music_ui.cover_image);
}

static void music_lyrics_render(size_t active_line)
{
    const bool has_lines = s_music_ui.lyric_line_count > 0U;
    for (size_t index = 0U; index < 5U; ++index)
    {
        const int32_t line = static_cast<int32_t>(active_line) +
                             static_cast<int32_t>(index) - 2;
        const char *text = s_fallback_lyrics[index];
        if (has_lines)
        {
            text = (line >= 0 &&
                    static_cast<size_t>(line) < s_music_ui.lyric_line_count)
                       ? s_music_ui.lyric_text_lines[line]
                       : "";
        }
        lv_label_set_text(s_music_ui.lyric_lines[index], text);
        lv_obj_set_style_text_color(s_music_ui.lyric_lines[index],
                                    index == 2U ? lv_color_hex(CYBER_YELLOW_GREEN)
                                                : lv_color_hex(CYBER_MUTED), 0);
    }
}

static void music_lyrics_set(const char *lyrics)
{
    s_music_ui.lyric_line_count = 0U;
    memset(s_music_ui.lyric_text_lines, 0, sizeof(s_music_ui.lyric_text_lines));
    memset(s_music_ui.lyric_line_times, 0, sizeof(s_music_ui.lyric_line_times));
    if (lyrics != NULL && lyrics[0] != '\0')
    {
        const char *cursor = lyrics;
        while (*cursor != '\0' && s_music_ui.lyric_line_count < MUSIC_MAX_LYRICS_LINES)
        {
            const char *end = strchr(cursor, '\n');
            const size_t length = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
            size_t start = 0U;
            uint32_t timestamp = UINT32_MAX;
            if (length >= 4U && cursor[0] == '[')
            {
                const char *colon = static_cast<const char *>(memchr(cursor, ':', length));
                const char *close = static_cast<const char *>(memchr(cursor, ']', length));
                if (colon != NULL && close != NULL && colon > cursor + 1 && close > colon)
                {
                    const uint32_t minutes = static_cast<uint32_t>(strtoul(cursor + 1, NULL, 10));
                    const uint32_t seconds = static_cast<uint32_t>(strtoul(colon + 1, NULL, 10));
                    timestamp = minutes * 60000U + seconds * 1000U;
                    start = static_cast<size_t>(close - cursor + 1);
                }
            }
            const size_t text_length = length > start ? length - start : 0U;
            const size_t copy = text_length < MUSIC_NAME_SIZE - 1U ? text_length : MUSIC_NAME_SIZE - 1U;
            memcpy(s_music_ui.lyric_text_lines[s_music_ui.lyric_line_count], cursor + start, copy);
            if (copy > 0U && s_music_ui.lyric_text_lines[s_music_ui.lyric_line_count][copy - 1U] == '\r')
                s_music_ui.lyric_text_lines[s_music_ui.lyric_line_count][copy - 1U] = '\0';
            s_music_ui.lyric_line_times[s_music_ui.lyric_line_count] = timestamp;
            ++s_music_ui.lyric_line_count;
            cursor = end != NULL ? end + 1U : cursor + length;
        }
    }
    music_lyrics_render(0U);
}

static void music_lyrics_update(uint32_t position_ms, uint32_t duration_ms)
{
    if (s_music_ui.lyric_line_count == 0U)
        return;
    size_t active = 0U;
    bool has_timestamps = s_music_ui.lyric_line_times[0] != UINT32_MAX;
    if (has_timestamps)
    {
        for (size_t index = 0U; index < s_music_ui.lyric_line_count; ++index)
        {
            if (s_music_ui.lyric_line_times[index] == UINT32_MAX)
            {
                has_timestamps = false;
                break;
            }
            if (position_ms >= s_music_ui.lyric_line_times[index])
                active = index;
        }
    }
    if (!has_timestamps && duration_ms > 0U)
        active = (position_ms * s_music_ui.lyric_line_count) / duration_ms;
    if (active >= s_music_ui.lyric_line_count)
        active = s_music_ui.lyric_line_count - 1U;
    music_lyrics_render(active);
}

static void music_show_track_info(void)
{
    music_set_fallback_metadata();
    music_cover_release();
    s_music_ui.lyrics_text[0] = '\0';
    if (s_music_ui.track_count > 0U)
    {
        uint8_t *cover = NULL;
        size_t cover_size = 0U;
        (void)music_audio_load_metadata(music_current_path(), &cover, &cover_size,
                                        s_music_ui.lyrics_text, sizeof(s_music_ui.lyrics_text));
        if (cover != NULL)
        {
            s_music_ui.cover_buf = music_decode_jpeg(cover, cover_size);
            music_audio_free_cover(cover);
            if (s_music_ui.cover_buf != NULL)
                lv_image_set_src(s_music_ui.cover_image, s_music_ui.cover_buf);
        }
    }
    if (s_music_ui.cover_buf != NULL)
        lv_obj_add_flag(s_music_ui.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(s_music_ui.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_music_ui.title, s_music_ui.title_text);
    lv_label_set_text(s_music_ui.artist, s_music_ui.artist_text);
    lv_label_set_text(s_music_ui.album, s_music_ui.album_text);
    music_lyrics_set(s_music_ui.lyrics_text);
    s_music_ui.displayed_track_index = s_music_ui.current_index;
    s_music_ui.displayed_track_valid = s_music_ui.track_count > 0U;
}

static void music_playlist_refresh(void);
static void music_playlist_update_item(size_t slot, size_t index);

static void music_label_set_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL)
        return;
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0)
        lv_label_set_text(label, text);
}

static const char *music_order_mode_text(music_order_mode_t mode)
{
    return mode == MUSIC_ORDER_SEQUENCE
               ? "SEQ"
           : mode == MUSIC_ORDER_SHUFFLE
               ? "RND"
               : "ONE";
}

static void music_audio_params_sync(void)
{
    if (s_music_ui.audio_params_primary == NULL ||
        s_music_ui.audio_params_secondary == NULL)
        return;

    music_audio_format_t format{};
    music_audio_get_format(&format);
    char primary[32];
    char secondary[32];
    if (format.sample_rate_hz > 0)
    {
        const int32_t rate_khz = format.sample_rate_hz / 1000;
        const int32_t rate_fraction = (format.sample_rate_hz % 1000) / 100;
        snprintf(primary, sizeof(primary), "%-6s %2ld.%01ld KHZ",
                 format.codec != NULL ? format.codec : "AUDIO",
                 static_cast<long>(rate_khz),
                 static_cast<long>(rate_fraction));
    }
    else
    {
        snprintf(primary, sizeof(primary), "--  --.- KHZ");
    }

    if (format.bitrate_bps > 0 || format.channels > 0 || format.bits_per_sample > 0)
    {
        snprintf(secondary, sizeof(secondary), "%3ldK  %2uCH  %2uBIT",
                 static_cast<long>(format.bitrate_bps > 0 ? format.bitrate_bps / 1000 : 0),
                 static_cast<unsigned>(format.channels),
                 static_cast<unsigned>(format.bits_per_sample));
    }
    else
    {
        snprintf(secondary, sizeof(secondary), "---K  --  --BIT");
    }
    music_label_set_if_changed(s_music_ui.audio_params_primary, primary);
    music_label_set_if_changed(s_music_ui.audio_params_secondary, secondary);
}

static void music_select_track(size_t index, bool autoplay, bool refresh_playlist = true)
{
    if (index >= s_music_ui.track_count)
        return;
    portENTER_CRITICAL(&s_music_playlist_lock);
    s_music_ui.current_index = index;
    portEXIT_CRITICAL(&s_music_playlist_lock);
    lv_obj_clear_flag(s_music_ui.cover_placeholder, LV_OBJ_FLAG_HIDDEN);
    lv_slider_set_range(s_music_ui.progress, 0, 1);
    lv_slider_set_value(s_music_ui.progress, 0, LV_ANIM_OFF);
    if (refresh_playlist)
        music_playlist_refresh();
    if (autoplay)
        music_audio_play(music_current_path());
    /* The decoder may still report the previous track immediately after a
     * queued switch. Use the duration probed during playlist scan here; the
     * live decoder duration is applied by the playback timer once available. */
    music_ensure_track_duration(index);
    const uint32_t duration = s_music_ui.tracks[index].duration_ms;
    if (duration > 0U)
        lv_slider_set_range(s_music_ui.progress, 0, (int32_t)duration);
    char duration_text[12] = "00:00";
    music_format_time(duration, duration_text, sizeof(duration_text));
    lv_label_set_text(s_music_ui.elapsed, "00:00");
    lv_label_set_text(s_music_ui.duration, duration_text);
    music_show_track_info();
    if (refresh_playlist)
        music_playlist_refresh();
    music_set_play_symbol();
}

static size_t music_next_index(bool previous)
{
    if (s_music_ui.track_count == 0U || s_music_ui.order_mode == MUSIC_ORDER_REPEAT)
        return s_music_ui.current_index;
    if (s_music_ui.order_mode == MUSIC_ORDER_SHUFFLE && s_music_ui.track_count > 1U)
    {
        size_t index = s_music_ui.current_index;
        while (index == s_music_ui.current_index)
            index = (size_t)(esp_random() % s_music_ui.track_count);
        return index;
    }
    if (previous)
        return s_music_ui.current_index == 0U
                   ? s_music_ui.track_count - 1U
                   : s_music_ui.current_index - 1U;
    return (s_music_ui.current_index + 1U) % s_music_ui.track_count;
}

static void music_previous_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (s_music_ui.track_count == 0U)
        return;
    music_select_track(music_next_index(true), true);
}

static void music_next_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (s_music_ui.track_count == 0U)
        return;
    music_select_track(music_next_index(false), true);
}

static void music_order_clicked_cb(lv_event_t *event)
{
    (void)event;
    portENTER_CRITICAL(&s_music_playlist_lock);
    s_music_ui.order_mode = (music_order_mode_t)((s_music_ui.order_mode + 1U) % 3U);
    const music_order_mode_t order_mode = s_music_ui.order_mode;
    portEXIT_CRITICAL(&s_music_playlist_lock);
    lv_label_set_text(s_music_ui.order_label, music_order_mode_text(order_mode));
}

static void music_playback_state_timer(lv_timer_t *timer)
{
    const music_audio_state_t state = music_audio_state();
    if (s_music_ui.displayed_track_valid && s_music_ui.track_count > 0U)
    {
        size_t current_index = 0U;
        portENTER_CRITICAL(&s_music_playlist_lock);
        current_index = s_music_ui.current_index;
        portEXIT_CRITICAL(&s_music_playlist_lock);
        if (current_index < s_music_ui.track_count &&
            current_index != s_music_ui.displayed_track_index)
        {
            /* The player task advances the queue when a song finishes. Sync
             * all track-bound UI from the LVGL task, including cover, title,
             * album, lyrics, duration and playlist highlight. */
            music_select_track(current_index, false, true);
        }
    }
    if (s_music_ui.progress != NULL && s_music_ui.elapsed != NULL &&
        !lv_slider_is_dragged(s_music_ui.progress))
    {
        const uint32_t position = music_audio_position_ms();
        const uint32_t duration = music_audio_duration_ms();
        if (duration > 0U)
        {
            if (lv_slider_get_max_value(s_music_ui.progress) != (int32_t)duration)
                lv_slider_set_range(s_music_ui.progress, 0, (int32_t)duration);
            lv_slider_set_value(s_music_ui.progress,
                                position > duration ? duration : position,
                                LV_ANIM_OFF);
            char duration_text[12];
            music_format_time(duration, duration_text, sizeof(duration_text));
            lv_label_set_text(s_music_ui.duration, duration_text);
        }
        char elapsed_text[12];
        music_format_time(position, elapsed_text, sizeof(elapsed_text));
        lv_label_set_text(s_music_ui.elapsed, elapsed_text);
    }
    const uint32_t now = lv_tick_get();
    if (state == MUSIC_AUDIO_STATE_PLAYING &&
        (s_diag_last_ms == 0U || now - s_diag_last_ms >= 5000U))
    {
        bsp::t_panel_p4_standard::Board *board = factory::services::board();
        if (board == NULL)
            return;
        if (s_diag_last_ms == 0U)
        {
            s_diag_last_ms = now;
            s_diag_last_position_ms = music_audio_position_ms();
            s_diag_last_music_stats = board->musicStatistics();
            s_diag_last_audio_stats = board->audioStatistics();
        }
        else
        {
            const uint32_t interval_ms = now - s_diag_last_ms;
            const uint32_t position_ms = music_audio_position_ms();
            const auto music_stats = board->musicStatistics();
            const auto audio_stats = board->audioStatistics();
            music_audio_format_t format{};
            music_audio_get_format(&format);
            ESP_LOGI(kMusicPageTag,
                     "interval=%lums pos_delta=%ldms out=%luB/s out_call=%lu short=%lu err=%lu timeout=%lu I2S_timeout=%lu err=%lu short=%lu fmt=%s/%ldHz/%lubit/%uch",
                     static_cast<unsigned long>(interval_ms),
                     static_cast<long>(position_ms - s_diag_last_position_ms),
                     static_cast<unsigned long>((music_stats.output_bytes - s_diag_last_music_stats.output_bytes) * 1000U / interval_ms),
                     static_cast<unsigned long>(music_stats.output_calls - s_diag_last_music_stats.output_calls),
                     static_cast<unsigned long>(music_stats.output_short_writes - s_diag_last_music_stats.output_short_writes),
                     static_cast<unsigned long>(music_stats.output_errors - s_diag_last_music_stats.output_errors),
                     static_cast<unsigned long>(music_stats.playback_timeouts - s_diag_last_music_stats.playback_timeouts),
                     static_cast<unsigned long>(audio_stats.tx_timeouts - s_diag_last_audio_stats.tx_timeouts),
                     static_cast<unsigned long>(audio_stats.tx_errors - s_diag_last_audio_stats.tx_errors),
                     static_cast<unsigned long>(audio_stats.tx_short_writes - s_diag_last_audio_stats.tx_short_writes),
                     format.codec ? format.codec : "?",
                     static_cast<long>(format.sample_rate_hz),
                     static_cast<unsigned long>(format.bitrate_bps),
                     static_cast<unsigned>(format.channels));
            s_diag_last_ms = now;
            s_diag_last_position_ms = position_ms;
            s_diag_last_music_stats = music_stats;
            s_diag_last_audio_stats = audio_stats;
        }
    }
    else if (state != MUSIC_AUDIO_STATE_PLAYING)
    {
        s_diag_last_ms = 0U;
    }
    (void)timer;
    const bool active = state == MUSIC_AUDIO_STATE_PLAYING && !music_audio_is_paused();
    music_energy_update(active);
    int16_t samples[MUSIC_SPECTRUM_SAMPLES]{};
    music_audio_get_waveform(samples, MUSIC_SPECTRUM_SAMPLES);
    music_lyrics_update(music_audio_position_ms(), music_audio_duration_ms());
    music_audio_params_sync();
    for (size_t index = 0U; index < MUSIC_SPECTRUM_BARS; ++index)
    {
        float real = 0.0F;
        float imaginary = 0.0F;
        for (size_t sample = 0U; sample < MUSIC_SPECTRUM_SAMPLES; ++sample)
        {
            const size_t phase = ((index + 1U) * sample) % MUSIC_SPECTRUM_SAMPLES;
            real += (float)samples[sample] * s_spectrum_cos[phase];
            imaginary -= (float)samples[sample] * s_spectrum_sin[phase];
        }
        const float magnitude = sqrtf(real * real + imaginary * imaginary) /
                                (float)(MUSIC_SPECTRUM_SAMPLES * 32768U);
        const float target_level = active
                                       ? 8.0F + fminf(1.0F, magnitude * 4.0F) * (MUSIC_SPECTRUM_HEIGHT - 12.0F)
                                       : 4.0F;
        const float attack = 0.42F;
        const float release = 0.16F;
        const float delta = target_level - s_music_ui.spectrum_display[index];
        s_music_ui.spectrum_display[index] += delta * (delta > 0.0F ? attack : release);
        const uint8_t level = static_cast<uint8_t>(fmaxf(4.0F, s_music_ui.spectrum_display[index]));
        s_music_ui.spectrum_levels[index] = level;
        if (s_music_ui.spectrum_bars[index] != NULL)
        {
            lv_obj_set_height(s_music_ui.spectrum_bars[index], level);
            lv_obj_align(s_music_ui.spectrum_bars[index], LV_ALIGN_BOTTOM_LEFT,
                         6 + (lv_coord_t)(index * 12U), 0);
        }
    }
    music_set_play_symbol();
}

static void music_play_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (music_audio_is_playing())
    {
        music_audio_pause(!music_audio_is_paused());
    }
    else
    {
        if (s_music_ui.track_count > 0U)
            music_select_track(s_music_ui.current_index, true);
    }
    music_set_play_symbol();
}

static void music_progress_changed_cb(lv_event_t *event)
{
    if (s_music_ui.progress == NULL || s_music_ui.elapsed == NULL)
        return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED)
        return;

    const int32_t value = lv_slider_get_value(s_music_ui.progress);
    if (value < 0)
        return;

    /* Update the time label while dragging, but seek only after release. */
    char elapsed_text[12];
    music_format_time((uint32_t)value, elapsed_text, sizeof(elapsed_text));
    lv_label_set_text(s_music_ui.elapsed, elapsed_text);
    if (code == LV_EVENT_RELEASED)
    {
        (void)music_audio_seek_ms((uint32_t)value);
    }
}

static int32_t music_volume_from_pointer(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL || s_music_ui.volume == NULL)
        return lv_slider_get_value(s_music_ui.volume);

    lv_point_t point;
    lv_area_t area;
    lv_indev_get_point(indev, &point);
    lv_obj_get_coords(s_music_ui.volume, &area);
    const int32_t width = area.x2 - area.x1 + 1;
    if (width <= 0)
        return lv_slider_get_value(s_music_ui.volume);

    const int32_t offset = LV_CLAMP(0, point.x - area.x1, width);
    return (offset * 100 + width / 2) / width;
}

static void music_volume_sync_text(void)
{
    if (s_music_ui.volume == NULL || s_music_ui.volume_text == NULL)
        return;
    const int32_t value = lv_slider_get_value(s_music_ui.volume);
    char text[8];
    snprintf(text, sizeof(text), "%ld%%", (long)value);
    lv_label_set_text(s_music_ui.volume_text, text);
}

static void music_volume_changed_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED)
    {
        lv_slider_set_value(s_music_ui.volume, music_volume_from_pointer(), LV_ANIM_OFF);
        music_volume_sync_text();
    }

    if (code == LV_EVENT_VALUE_CHANGED)
        music_volume_sync_text();
    if (code == LV_EVENT_RELEASED)
    {
        music_audio_set_volume_percent(lv_slider_get_value(s_music_ui.volume));
    }
}

static void music_playlist_toggle_cb(lv_event_t *event)
{
    (void)event;
    if (s_music_ui.playlist == NULL)
        return;
    if (lv_obj_has_flag(s_music_ui.playlist, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(s_music_ui.playlist, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_music_ui.playlist, LV_OBJ_FLAG_HIDDEN);
}

static void music_playlist_close_cb(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_music_ui.playlist, LV_OBJ_FLAG_HIDDEN);
}

static void music_playlist_item_cb(lv_event_t *event)
{
    const size_t slot = (size_t)(uintptr_t)lv_event_get_user_data(event);
    const lv_coord_t scroll_y = lv_obj_get_scroll_y(s_music_ui.playlist_list);
    const size_t first = scroll_y > 0 ? (size_t)scroll_y / 74U : 0U;
    const size_t index = first + slot;
    if (index >= s_music_ui.track_count)
        return;
    lv_obj_add_flag(s_music_ui.playlist, LV_OBJ_FLAG_HIDDEN);
    music_select_track(index, true);
}

static void music_playlist_scroll_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCROLL)
        music_playlist_refresh();
}

static void music_cover_create(lv_obj_t *parent)
{
    s_music_ui.cover = lv_obj_create(parent);
    lv_obj_set_size(s_music_ui.cover, MUSIC_COVER_SIZE, MUSIC_COVER_SIZE);
    lv_obj_set_pos(s_music_ui.cover, MUSIC_COVER_X, MUSIC_COVER_Y);
    page_ui_apply_panel_style(s_music_ui.cover, UI_PANEL);
    lv_obj_set_style_bg_color(s_music_ui.cover, lv_color_hex(CYBER_PANEL), 0);
    lv_obj_set_style_border_width(s_music_ui.cover, 0, 0);
    lv_obj_set_style_radius(s_music_ui.cover, 0, 0);
    lv_obj_set_style_pad_all(s_music_ui.cover, 0, 0);
    lv_obj_remove_flag(s_music_ui.cover, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    music_gesture_passthrough(s_music_ui.cover);
    lv_obj_set_style_shadow_width(s_music_ui.cover, 18, 0);
    lv_obj_set_style_shadow_spread(s_music_ui.cover, 2, 0);
    lv_obj_set_style_shadow_color(s_music_ui.cover, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_shadow_opa(s_music_ui.cover, LV_OPA_40, 0);

    s_music_ui.cover_image = lv_image_create(s_music_ui.cover);
    lv_obj_set_size(s_music_ui.cover_image, MUSIC_COVER_SIZE, MUSIC_COVER_SIZE);
    lv_obj_set_style_radius(s_music_ui.cover_image, 0, 0);
    lv_image_set_inner_align(s_music_ui.cover_image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_center(s_music_ui.cover_image);
    music_gesture_passthrough(s_music_ui.cover_image);

    s_music_ui.cover_placeholder = page_ui_label(
        s_music_ui.cover, LV_SYMBOL_AUDIO, &lv_font_ShareTechMono1_50, CYBER_YELLOW_GREEN);
    lv_obj_center(s_music_ui.cover_placeholder);
    lv_obj_set_style_opa(s_music_ui.cover_placeholder, LV_OPA_80, 0);
    music_gesture_passthrough(s_music_ui.cover_placeholder);

    page_ui_add_top_left_bottom_right_brackets(
        s_music_ui.cover, MUSIC_COVER_SIZE, MUSIC_COVER_SIZE, CYBER_YELLOW, 6);
}

static void music_playlist_create(lv_obj_t *parent)
{
    s_music_ui.playlist = lv_obj_create(parent);
    lv_obj_set_size(s_music_ui.playlist, MUSIC_LIST_WIDTH, MUSIC_LIST_HEIGHT);
    lv_obj_set_pos(s_music_ui.playlist, MUSIC_LIST_X, MUSIC_LIST_Y);
    page_ui_apply_panel_style(s_music_ui.playlist, CYBER_PANEL);
    lv_obj_set_style_border_color(s_music_ui.playlist, lv_color_hex(CYBER_MAGENTA), 0);
    lv_obj_set_style_border_width(s_music_ui.playlist, 0, 0);
    lv_obj_set_style_pad_all(s_music_ui.playlist, 22, 0);
    lv_obj_set_scrollbar_mode(s_music_ui.playlist, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_music_ui.playlist, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(s_music_ui.playlist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_music_ui.playlist, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *heading = page_ui_label(
        s_music_ui.playlist, "PlayList", &lv_font_ShareTechMono1_24, CYBER_CYAN);
    lv_obj_set_pos(heading, 0, 0);
    lv_obj_t *close = music_button(
        s_music_ui.playlist, LV_SYMBOL_CLOSE, 44, 40,
        music_playlist_close_cb, NULL);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_radius(close, 0, 0);
    page_ui_add_top_left_bottom_right_brackets(close, 44, 40, CYBER_YELLOW_GREEN, 0);

    s_music_ui.playlist_list = lv_obj_create(s_music_ui.playlist);
    lv_obj_set_size(s_music_ui.playlist_list, MUSIC_LIST_WIDTH - 44, MUSIC_LIST_HEIGHT - 92);
    lv_obj_set_pos(s_music_ui.playlist_list, 0, 62);
    lv_obj_set_style_bg_opa(s_music_ui.playlist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_music_ui.playlist_list, 0, 0);
    lv_obj_set_style_pad_all(s_music_ui.playlist_list, 0, 0);
    lv_obj_set_scrollbar_mode(s_music_ui.playlist_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_music_ui.playlist_list, LV_DIR_VER);
    lv_obj_add_flag(s_music_ui.playlist_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_music_ui.playlist_list, music_playlist_scroll_cb, LV_EVENT_SCROLL, NULL);
    s_music_ui.playlist_spacer = lv_obj_create(s_music_ui.playlist_list);
    lv_obj_set_pos(s_music_ui.playlist_spacer, 0, 0);
    lv_obj_set_size(s_music_ui.playlist_spacer, 1, 1);
    lv_obj_set_style_bg_opa(s_music_ui.playlist_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_music_ui.playlist_spacer, 0, 0);
    lv_obj_remove_flag(s_music_ui.playlist_spacer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_music_ui.playlist_spacer, LV_OBJ_FLAG_SCROLLABLE);
    for (size_t slot = 0U; slot < MUSIC_PLAYLIST_ROW_CACHE; ++slot)
    {
        lv_obj_t *item = lv_obj_create(s_music_ui.playlist_list);
        lv_obj_set_size(item, MUSIC_LIST_WIDTH - 44, 68);
        lv_obj_set_pos(item, 0, (lv_coord_t)(slot * 74U));
        lv_obj_set_style_bg_color(item, lv_color_hex(CYBER_YELLOW_GREEN), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(item, LV_OPA_40, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_scrollbar_mode(item, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(item, music_playlist_item_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)slot);

        lv_obj_t *number = lv_obj_create(item);
        lv_obj_set_size(number,4, 48);
        lv_obj_set_style_bg_opa(number, LV_OPA_100, 0);
        lv_obj_set_style_radius(number, 0, 0);
        lv_obj_set_style_bg_color(number, lv_color_hex(CYBER_CYAN), 0);
        lv_obj_align(number, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_border_width(number, 0, 0);
        lv_obj_set_style_pad_all(number, 0, 0);

        lv_obj_t *title = page_ui_label(
            item, "", &lv_font_source_han_sans_sc_16_cjk, CYBER_TEXT);
        lv_obj_set_width(title, MUSIC_LIST_WIDTH - 174);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(title, 44, 8);
        lv_obj_t *artist = page_ui_label(
            item, "", &lv_font_source_han_sans_sc_16_cjk, CYBER_MUTED);
        lv_obj_set_width(artist, MUSIC_LIST_WIDTH - 174);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(artist, 44, 38);
        lv_obj_t *duration = page_ui_label(
            item, "--:--", &lv_font_ShareTechMono1_16, CYBER_CYAN);
        lv_obj_set_width(duration, 58);
        lv_obj_set_style_text_align(duration, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(duration, LV_ALIGN_RIGHT_MID, -12, 0);
        s_music_ui.playlist_rows[slot] = item;
    }
    music_playlist_refresh();
}

static void music_playlist_update_item(size_t slot, size_t index)
{
    if (s_music_ui.playlist_list == NULL || slot >= MUSIC_PLAYLIST_ROW_CACHE)
        return;
    lv_obj_t *item = s_music_ui.playlist_rows[slot];
    if (item == NULL)
        return;
    if (index >= s_music_ui.track_count)
    {
        lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(item, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(item, (lv_coord_t)(index * 74U));
    if (lv_obj_get_child_count(item) < 4U)
        return;
    lv_obj_set_style_bg_color(
        item, index == s_music_ui.current_index ? lv_color_hex(0x123523) : lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_border_color(
        item, index == s_music_ui.current_index ? lv_color_hex(CYBER_YELLOW_GREEN) : lv_color_hex(CYBER_GRID), 0);
    lv_obj_t *number = lv_obj_get_child(item, 0);
    if (number != NULL)
    {
        lv_obj_set_style_bg_opa(number, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(
            number,
            lv_color_hex(index == s_music_ui.current_index
                             ? CYBER_YELLOW_GREEN
                             : CYBER_CYAN),
            0);
    }
    music_ensure_track_duration(index);
    music_label_set_if_changed(lv_obj_get_child(item, 1), s_music_ui.tracks[index].title);
    music_label_set_if_changed(lv_obj_get_child(item, 2), s_music_ui.tracks[index].artist);
    char duration_text[12] = "--:--";
    if (s_music_ui.tracks[index].duration_ms > 0U)
        music_format_time(s_music_ui.tracks[index].duration_ms,
                          duration_text, sizeof(duration_text));
    music_label_set_if_changed(lv_obj_get_child(item, 3), duration_text);
}

static void music_playlist_refresh(void)
{
    if (s_music_ui.playlist_list == NULL)
        return;
    const lv_coord_t scroll_y = lv_obj_get_scroll_y(s_music_ui.playlist_list);
    const size_t first = scroll_y > 0 ? (size_t)scroll_y / 74U : 0U;
    for (size_t slot = 0U; slot < MUSIC_PLAYLIST_ROW_CACHE; ++slot)
        music_playlist_update_item(slot, first + slot);
    const size_t content_rows = s_music_ui.track_count > MUSIC_PLAYLIST_ROW_CACHE
                                    ? s_music_ui.track_count
                                    : MUSIC_PLAYLIST_ROW_CACHE;
    if (s_music_ui.playlist_spacer != NULL)
        lv_obj_set_height(s_music_ui.playlist_spacer,
                          (lv_coord_t)(content_rows * 74U));
}

static void music_page_destroy(Page *page)
{
    (void)page;
    if (s_music_ui.timer != NULL)
    {
        lv_timer_del(s_music_ui.timer);
        s_music_ui.timer = NULL;
    }
    music_cover_release();

    // Keep only playlist metadata for the player task. The LVGL tree and its
    // timer are deleted by the page manager, while playback remains active.
    portENTER_CRITICAL(&s_music_playlist_lock);
    music_track_t *tracks = s_music_ui.tracks;
    const size_t track_count = s_music_ui.track_count;
    const size_t current_index = s_music_ui.current_index;
    const bool playlist_scanned = s_music_ui.playlist_scanned;
    const music_order_mode_t order_mode = s_music_ui.order_mode;
    memset(&s_music_ui, 0, sizeof(s_music_ui));
    s_music_ui.tracks = tracks;
    s_music_ui.track_count = track_count;
    s_music_ui.current_index = current_index;
    s_music_ui.playlist_scanned = playlist_scanned;
    s_music_ui.order_mode = order_mode;
    portEXIT_CRITICAL(&s_music_playlist_lock);
}

static void music_page_enter(Page *page)
{
    (void)page;
    // Show component-level open/format/warning/error messages while diagnosing
    // intermittent playback stalls. INFO avoids per-read DEBUG log flooding.
    for (const char *tag : kMusicComponentTags)
        esp_log_level_set(tag, ESP_LOG_INFO);
    (void)music_audio_enter();

    char current_path[MUSIC_PATH_SIZE];
    bool track_shown = false;
    if (music_audio_current_path(current_path, sizeof(current_path)))
    {
        for (size_t index = 0U; index < s_music_ui.track_count; ++index)
        {
            if (strcmp(s_music_ui.tracks[index].path, current_path) != 0)
                continue;
            music_select_track(index, false, false);
            track_shown = true;
            break;
        }
    }
    if (!track_shown)
        music_show_track_info();
    /* Rebind the newly-created cached rows after the page tree is rebuilt. */
    music_playlist_refresh();
    music_playback_state_timer(NULL);
    music_set_play_symbol();
}

static void music_page_leave(Page *page)
{
    (void)page;
    // Page switching does not destroy pages automatically. Release the
    // music UI tree and playlist here, but keep the board-level music player
    // and its playback task running.
    (void)ui_page_destroy(PAGE_MUSIC);
}

static void music_page_create(Page *page)
{
    s_music_ui.root = page->root;
    if (music_buffers_allocate())
    {
        if (!s_music_ui.playlist_scanned || s_music_ui.track_count == 0U)
            music_scan_directory();
    }
    else
        s_music_ui.track_count = 0U;
    if (factory::services::board() != NULL)
        (void)factory::services::board()->setMusicNextTrackCallback(
            music_next_track_for_player, NULL);
    lv_obj_set_style_bg_color(s_music_ui.root, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_pad_all(s_music_ui.root, 0, 0);
    lv_obj_set_scrollbar_mode(s_music_ui.root, LV_SCROLLBAR_MODE_OFF);

    music_cover_create(s_music_ui.root);

    s_music_ui.title = page_ui_label(
        s_music_ui.root, "LilyGo Demo", &lv_font_source_han_sans_sc_16_cjk, CYBER_MAGENTA);
    lv_obj_set_width(s_music_ui.title, MUSIC_INFO_WIDTH);
    lv_label_set_long_mode(s_music_ui.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_music_ui.title, MUSIC_INFO_X, MUSIC_INFO_Y);

    s_music_ui.artist = page_ui_label(
        s_music_ui.root, "Unknown artist", &lv_font_source_han_sans_sc_16_cjk, CYBER_CYAN);
    lv_obj_set_width(s_music_ui.artist, MUSIC_INFO_WIDTH);
    lv_label_set_long_mode(s_music_ui.artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_music_ui.artist, MUSIC_INFO_X, MUSIC_INFO_Y + 36);
    lv_obj_set_style_text_color(s_music_ui.artist, lv_color_hex(CYBER_CYAN), 0);
    s_music_ui.album = page_ui_label(
        s_music_ui.root, "Unknow", &lv_font_source_han_sans_sc_16_cjk, CYBER_MUTED);
    lv_obj_set_width(s_music_ui.album, MUSIC_COVER_SIZE);
    lv_label_set_long_mode(s_music_ui.album, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_music_ui.album, MUSIC_COVER_X,
                   MUSIC_COVER_Y + MUSIC_COVER_SIZE + 16);
    music_spectrum_create(s_music_ui.root);
    music_energy_create(s_music_ui.root);
    s_music_ui.audio_params_primary = page_ui_label(
        s_music_ui.root, "--  --.- KHZ", &lv_font_ShareTechMono1_16, CYBER_CYAN);
    lv_obj_set_width(s_music_ui.audio_params_primary, MUSIC_SPECTRUM_WIDTH);
    lv_obj_set_height(s_music_ui.audio_params_primary, 18);
    lv_obj_set_pos(s_music_ui.audio_params_primary,
                   MUSIC_SPECTRUM_X, MUSIC_VOLUME_Y - 23);
    s_music_ui.audio_params_secondary = page_ui_label(
        s_music_ui.root, "---K  --  --BIT", &lv_font_ShareTechMono1_16, CYBER_YELLOW_GREEN);
    lv_obj_set_width(s_music_ui.audio_params_secondary, MUSIC_SPECTRUM_WIDTH);
    lv_obj_set_height(s_music_ui.audio_params_secondary, 18);
    lv_obj_set_pos(s_music_ui.audio_params_secondary,
                   MUSIC_SPECTRUM_X,
                   MUSIC_VOLUME_Y + MUSIC_VOLUME_HEIGHT - 18);
    lv_obj_t *lyric_container = lv_obj_create(s_music_ui.root);
    lv_obj_set_size(lyric_container, MUSIC_INFO_WIDTH, 200);
    lv_obj_set_pos(lyric_container, MUSIC_INFO_X, MUSIC_LYRICS_Y);
    lv_obj_set_style_border_width(lyric_container, 6, 0);
    lv_obj_set_style_border_side(lyric_container, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(lyric_container, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_bg_color(lyric_container, lv_color_hex(CYBER_CYAN), 0);
    lv_obj_set_style_bg_grad_color(lyric_container, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_bg_grad_dir(lyric_container, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(lyric_container, LV_OPA_20, 0);
    lv_obj_set_style_radius(lyric_container, 0, 0);
    lv_obj_set_style_pad_all(lyric_container, 0, 0);
    music_gesture_passthrough(lyric_container);

    static const lv_opa_t lyric_opacity[5] = {
        LV_OPA_30, LV_OPA_60, LV_OPA_COVER, LV_OPA_60, LV_OPA_30};
    for (size_t index = 0; index < 5; ++index)
    {
        s_music_ui.lyric_lines[index] = page_ui_label(
            lyric_container, s_fallback_lyrics[index],
            &lv_font_source_han_sans_sc_16_cjk,
            index == 2 ? CYBER_YELLOW_GREEN : CYBER_MUTED);
        lv_obj_set_width(s_music_ui.lyric_lines[index], MUSIC_INFO_WIDTH - 24);
        lv_label_set_long_mode(s_music_ui.lyric_lines[index], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(s_music_ui.lyric_lines[index], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_opa(s_music_ui.lyric_lines[index], lyric_opacity[index], 0);
        lv_obj_set_pos(s_music_ui.lyric_lines[index], 12,
                       14 + (lv_coord_t)(index * 34));
    }

    s_music_ui.progress = lv_slider_create(s_music_ui.root);
    lv_obj_set_width(s_music_ui.progress, MUSIC_INFO_WIDTH);
    lv_obj_set_pos(s_music_ui.progress, MUSIC_INFO_X, MUSIC_PROGRESS_Y);
    lv_slider_set_range(s_music_ui.progress, 0, 1);
    lv_slider_set_value(s_music_ui.progress, 0, LV_ANIM_OFF);
    lv_obj_set_height(s_music_ui.progress, 5);
    lv_obj_set_style_bg_opa(s_music_ui.progress, 60, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_ui.progress, lv_color_hex(CYBER_GRID), LV_PART_MAIN);
    lv_obj_set_style_radius(s_music_ui.progress, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_ui.progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_music_ui.progress, lv_color_hex(CYBER_YELLOW_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_music_ui.progress, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_ui.progress, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_music_ui.progress, lv_color_hex(CYBER_YELLOW_GREEN), LV_PART_KNOB);
    lv_obj_set_style_radius(s_music_ui.progress, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s_music_ui.progress, music_progress_changed_cb, LV_EVENT_ALL, NULL);

    s_music_ui.elapsed = page_ui_label(s_music_ui.root, "00:00",
                                       &lv_font_ShareTechMono1_20, CYBER_CYAN);
    lv_obj_set_pos(s_music_ui.elapsed, MUSIC_INFO_X, MUSIC_PROGRESS_Y + 18);
    s_music_ui.duration = page_ui_label(s_music_ui.root, "00:00",
                                        &lv_font_ShareTechMono1_20, CYBER_CYAN);
    lv_obj_set_width(s_music_ui.duration, 70);
    lv_obj_set_style_text_align(s_music_ui.duration, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_music_ui.duration,
                   MUSIC_INFO_X + MUSIC_INFO_WIDTH - 70, MUSIC_PROGRESS_Y + 18);

    lv_obj_t *controls = lv_obj_create(s_music_ui.root);
    lv_obj_set_size(controls, MUSIC_INFO_WIDTH, 80);
    lv_obj_set_pos(controls, MUSIC_INFO_X, MUSIC_CONTROLS_Y);
    lv_obj_set_style_bg_opa(controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(controls, 0, 0);
    lv_obj_set_style_pad_all(controls, 0, 0);
    music_gesture_passthrough(controls);
    lv_obj_t *order = music_button(controls, "Seq", 60, 60,
                                   music_order_clicked_cb, NULL);
    lv_obj_set_pos(order, 0, 10);
    s_music_ui.order_label = lv_obj_get_child(order, 0);
    lv_label_set_text(s_music_ui.order_label,
                      music_order_mode_text(s_music_ui.order_mode));
    lv_obj_t *previous = music_button(controls, LV_SYMBOL_PREV, 60, 60,
                                      music_previous_clicked_cb, NULL);
    lv_obj_set_pos(previous, 95, 9);
    lv_obj_t *play = music_button(controls, LV_SYMBOL_PLAY, 80, 80,
                                  music_play_clicked_cb, NULL);
    lv_obj_set_pos(play, 171, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_bg_opa(play, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(play, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_shadow_color(play, lv_color_hex(CYBER_YELLOW_GREEN), 0);
    lv_obj_set_style_shadow_opa(play, LV_OPA_60, 0);
    s_music_ui.play_label = lv_obj_get_child(play, 0);
    s_music_ui.play_symbol_initialized = false;
    lv_obj_set_style_text_color(s_music_ui.play_label, lv_color_hex(CYBER_BG), 0);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_t *next = music_button(controls, LV_SYMBOL_NEXT, 60, 60,
                                  music_next_clicked_cb, NULL);
    lv_obj_set_pos(next, 267, 10);
    lv_obj_t *list = music_button(controls, LV_SYMBOL_LIST, 60, 60,
                                  music_playlist_toggle_cb, NULL);
    lv_obj_set_pos(list, MUSIC_INFO_WIDTH - 60, 10);

    s_music_ui.volume = lv_slider_create(s_music_ui.root);
    lv_obj_set_width(s_music_ui.volume, 300);
    lv_obj_set_pos(s_music_ui.volume, 326, MUSIC_VOLUME_Y);
    lv_slider_set_range(s_music_ui.volume, 0, 100);
    lv_slider_set_mode(s_music_ui.volume, LV_SLIDER_MODE_NORMAL);
    const uint8_t volume_percent = music_audio_get_volume_percent();
    lv_slider_set_value(s_music_ui.volume, volume_percent, LV_ANIM_OFF);
    lv_obj_set_height(s_music_ui.volume, MUSIC_VOLUME_HEIGHT);
    lv_obj_set_style_bg_opa(s_music_ui.volume, 60, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_music_ui.volume, lv_color_hex(CYBER_GRID), LV_PART_MAIN);
    lv_obj_set_style_radius(s_music_ui.volume, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_music_ui.volume, 193, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_music_ui.volume, lv_color_hex(CYBER_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_music_ui.volume, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_music_ui.volume, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_music_ui.volume, 18);
    lv_obj_add_event_cb(s_music_ui.volume, music_volume_changed_cb, LV_EVENT_ALL, NULL);
    lv_obj_t *volume_icon = page_ui_label(s_music_ui.root, LV_SYMBOL_VOLUME_MAX,
                                          &lv_font_ShareTechMono1_20, CYBER_CYAN);
    lv_obj_set_pos(volume_icon, 270, MUSIC_VOLUME_Y - 5);
    s_music_ui.volume_text = page_ui_label(s_music_ui.root, "0%",
                                           &lv_font_ShareTechMono1_16, CYBER_TEXT);
    lv_obj_set_width(s_music_ui.volume_text, 54);
    lv_obj_set_style_text_align(s_music_ui.volume_text, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_music_ui.volume_text,
                   MUSIC_INFO_X + MUSIC_INFO_WIDTH - 54, MUSIC_VOLUME_Y - 4);
    music_volume_sync_text();

    music_playlist_create(s_music_ui.root);
    music_set_fallback_metadata();
    lv_label_set_text(s_music_ui.title, s_music_ui.title_text);
    lv_label_set_text(s_music_ui.artist, s_music_ui.artist_text);
    lv_label_set_text(s_music_ui.album, s_music_ui.album_text);
    music_lyrics_set(NULL);
    music_spectrum_init_table();
    s_music_ui.timer = lv_timer_create(music_playback_state_timer, 33, NULL);
    music_playback_state_timer(NULL);
}

extern "C" void music_page_register(void)
{
    Page page{};

    page.id = PAGE_MUSIC;

    page.name = "music";

    page.on_create = music_page_create;

    page.on_destroy = music_page_destroy;

    page.on_enter = music_page_enter;

    page.on_leave = music_page_leave;

    page.on_gesture = page_ui_back_gesture;
    ui_page_register(&page);
}
