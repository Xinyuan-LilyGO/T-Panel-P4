#include "page_common.hpp"
#include "page_registry.hpp"
#include "services/record_audio.h"
#include "services/settings_control.h"
#include "services/service_context.hpp"
#include <esp_log.h>
#include <cstdio>
#include <cstring>

namespace factory::ui
{
    namespace
    {
        constexpr size_t kBars = 44;
        constexpr lv_coord_t kWaveHeight = 140;
        struct RecordUi
        {
            lv_obj_t *heading{}, *elapsed{}, *status{}, *waveform{}, *buffer_label{};
            lv_obj_t *record{}, *pause{}, *play{}, *volume{}, *volume_icon{}, *volume_min_icon{};
            lv_obj_t *bars[kBars]{};
            uint8_t heights[kBars]{};
            lv_timer_t *timer{};
            bool action_failed{};
        };

        RecordUi s_record;

        lv_obj_t *button(lv_obj_t *parent, const char *symbol, lv_coord_t size, lv_event_cb_t cb)
        {
            lv_obj_t *o = lv_obj_create(parent);
            if (!o)
                return nullptr;
            lv_obj_set_size(o, size, size);
            page_ui_apply_icon_style(o);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_opa(o, LV_OPA_30, LV_STATE_PRESSED);
            if (cb)
                lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_t *label = page_ui_label(o, symbol, &lv_font_montserrat_24, UI_WIHTE);
            if (label)
                lv_obj_center(label);
            return o;
        }
        void status(const char *text)
        {
            if (s_record.status && std::strcmp(lv_label_get_text(s_record.status), text) != 0)
                lv_label_set_text(s_record.status, text);
        }
        void clear_wave()
        {
            std::memset(s_record.heights, 0, sizeof(s_record.heights));
            for (size_t i = 0; i < kBars; ++i)
            {
                if (s_record.bars[i] == nullptr)
                    continue;
                lv_obj_set_height(s_record.bars[i], 4);
                lv_obj_set_y(s_record.bars[i], (kWaveHeight - 4) / 2);
                lv_obj_set_style_bg_opa(s_record.bars[i], LV_OPA_40, 0);
            }
        }
        void wave()
        {
            if ((!record_audio_is_recording() && !record_audio_is_playing()) ||
                (record_audio_is_recording() && record_audio_is_paused()))
                return;
            uint8_t previous_heights[kBars];
            std::memcpy(previous_heights, s_record.heights, sizeof(previous_heights));
            std::memmove(s_record.heights, s_record.heights + 1, kBars - 1);
            s_record.heights[kBars - 1] = record_audio_level_percent();
            for (size_t i = 0; i < kBars; ++i)
            {
                if (s_record.bars[i] == nullptr)
                    continue;
                lv_coord_t h = 4 + s_record.heights[i] * 124 / 100;
                const lv_coord_t previous_h = 4 + previous_heights[i] * 124 / 100;
                if (h == previous_h && (s_record.heights[i] > 4) == (previous_heights[i] > 4))
                    continue;
                lv_obj_set_height(s_record.bars[i], h);
                lv_obj_set_y(s_record.bars[i], (kWaveHeight - h) / 2);
                lv_obj_set_style_bg_opa(s_record.bars[i], s_record.heights[i] > 4 ? LV_OPA_COVER : LV_OPA_40, 0);
            }
        }
        void timer(lv_timer_t *)
        {
            if (s_record.elapsed)
            {
                uint32_t ms = record_audio_duration_ms();
                char t[16];
                std::snprintf(t, sizeof(t), "%02lu:%02lu:%02lu", (unsigned long)(ms / 3600000U), (unsigned long)((ms / 60000U) % 60U), (unsigned long)((ms / 1000U) % 60U));
                if (std::strcmp(lv_label_get_text(s_record.elapsed), t) != 0)
                    lv_label_set_text(s_record.elapsed, t);
            }
            const bool playing = record_audio_is_playing();
            if (s_record.play)
            {
                lv_obj_t *icon = lv_obj_get_child(s_record.play, 0);
                const char *symbol = playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY;
                if (std::strcmp(lv_label_get_text(icon), symbol) != 0)
                    lv_label_set_text(icon, symbol);
                if (record_audio_is_recording() || record_audio_cache_bytes() == 0U)
                    lv_obj_add_state(s_record.play, LV_STATE_DISABLED);
                else
                    lv_obj_remove_state(s_record.play, LV_STATE_DISABLED);
            }
            if (s_record.action_failed)
            {
                wave();
                return;
            }
            if (playing)
                status("Playing PSRAM recording");
            else if (!record_audio_is_recording() && record_audio_cache_bytes() != 0U)
            {
                status("Recording kept in PSRAM");
                lv_obj_t *record_icon = lv_obj_get_child(s_record.record, 0);
                lv_obj_t *pause_icon = lv_obj_get_child(s_record.pause, 0);
                if (std::strcmp(lv_label_get_text(record_icon), LV_SYMBOL_AUDIO) != 0)
                    lv_label_set_text(record_icon, LV_SYMBOL_AUDIO);
                if (std::strcmp(lv_label_get_text(pause_icon), LV_SYMBOL_PAUSE) != 0)
                    lv_label_set_text(pause_icon, LV_SYMBOL_PAUSE);
            }
            wave();
        }
        void record_cb(lv_event_t *)
        {
            ESP_LOGI("round_recorder_ui", "Record button clicked: recording=%d, playing=%d",
                     record_audio_is_recording(), record_audio_is_playing());
            s_record.action_failed = false;
            if (record_audio_is_recording())
            {
                if (record_audio_stop())
                {
                    status("Recording kept in PSRAM");
                    lv_label_set_text(lv_obj_get_child(s_record.record, 0), LV_SYMBOL_AUDIO);
                    lv_label_set_text(lv_obj_get_child(s_record.pause, 0), LV_SYMBOL_PAUSE);
                }
                else
                {
                    s_record.action_failed = true;
                    status("Unable to stop recording");
                }
                return;
            }
            if (!record_audio_stop_cached_playback())
            {
                s_record.action_failed = true;
                status("Unable to stop playback");
                return;
            }
            if (record_audio_start())
            {
                clear_wave();
                status("Recording");
                lv_label_set_text(lv_obj_get_child(s_record.record, 0), LV_SYMBOL_STOP);
                lv_label_set_text(lv_obj_get_child(s_record.pause, 0), LV_SYMBOL_PAUSE);
            }
            else
            {
                s_record.action_failed = true;
                status("Unable to start recording");
            }
        }
        void pause_cb(lv_event_t *)
        {
            if (!record_audio_is_recording())
                return;
            bool paused = !record_audio_is_paused();
            if (record_audio_pause(paused))
            {
                status(paused ? "Paused" : "Recording");
                lv_label_set_text(lv_obj_get_child(s_record.pause, 0), paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
            }
        }
        void play_cb(lv_event_t *)
        {
            if (record_audio_is_recording())
                return;
            s_record.action_failed = false;
            if (record_audio_is_playing())
            {
                if (!record_audio_stop_cached_playback())
                {
                    s_record.action_failed = true;
                    status("Unable to stop playback");
                    return;
                }
            }
            else
            {
                if (!record_audio_play_cached())
                {
                    s_record.action_failed = true;
                    status("Unable to play recording");
                    return;
                }
                clear_wave();
            }
            timer(nullptr);
        }
        void volume_changed(lv_event_t *event)
        {
            // Keep codec I/O out of the drag loop; the arc previews the value
            // itself, and the shared output volume is committed on release.
            if (event == nullptr || lv_event_get_code(event) != LV_EVENT_RELEASED ||
                s_record.volume == nullptr)
                return;

            lv_obj_t *arc = lv_event_get_target_obj(event);
            if (arc == nullptr)
                return;
            const int32_t value = lv_arc_get_value(arc);
            if (settings_control_set_audio_volume(static_cast<uint8_t>(value)) != ESP_OK)
            {
                uint8_t current{};
                if (settings_control_get_audio_volume(&current) == ESP_OK)
                    lv_arc_set_value(arc, current);
            }
        }
        void create(Page *page)
        {
            if (!page || !page->root)
                return;
            lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);
            lv_obj_remove_flag(page->root, LV_OBJ_FLAG_SCROLLABLE);
            ui_create_background(page->root);
            s_record.heading = page_ui_label(page->root, "Recorder", &lv_font_montserrat_20, UI_WIHTE);
            lv_obj_align(s_record.heading, LV_ALIGN_TOP_MID, 0, 90);
            s_record.elapsed = page_ui_label(page->root, "00:00:00", &lv_font_montserrat_32, UI_WIHTE);
            lv_obj_align(s_record.elapsed, LV_ALIGN_TOP_MID, 0, 180);
            s_record.status = page_ui_label(page->root, "Ready to record", &lv_font_montserrat_16, UI_GTAY);
            lv_obj_align(s_record.status, LV_ALIGN_TOP_MID, 0, 234);
            s_record.waveform = page_ui_container(page->root, 448, kWaveHeight);
            if (s_record.waveform)
            {
                lv_obj_align(s_record.waveform, LV_ALIGN_CENTER, 0, -4);
                lv_obj_set_style_bg_opa(s_record.waveform, LV_OPA_TRANSP, 0);
                lv_obj_remove_flag(s_record.waveform, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
                for (size_t i = 0; i < kBars; ++i)
                {
                    lv_obj_t *b = lv_obj_create(s_record.waveform);
                    if (!b)
                        continue;
                    // Wave bars have no theme padding, minimum size or transitions.
                    lv_obj_remove_style_all(b);
                    lv_obj_set_size(b, 4, 4);
                    lv_obj_set_pos(b, 7 + (lv_coord_t)i * 10, 68);
                    lv_obj_set_style_radius(b, 2, 0);
                    lv_obj_set_style_bg_color(b, lv_color_hex(UI_WIHTE), 0);
                    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
                    lv_obj_set_style_border_width(b, 0, 0);
                    lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
                    s_record.bars[i] = b;
                }
            }
            s_record.buffer_label = page_ui_label(page->root, "PSRAM buffer: 2 MiB", &lv_font_montserrat_14, UI_GTAY);
            lv_obj_align(s_record.buffer_label, LV_ALIGN_TOP_MID, 0, 458);
            s_record.record = button(page->root, LV_SYMBOL_AUDIO, 88, record_cb);
            lv_obj_align(s_record.record, LV_ALIGN_BOTTOM_MID, 0, -80);
            s_record.pause = button(page->root, LV_SYMBOL_PAUSE, 56, pause_cb);
            lv_obj_align(s_record.pause, LV_ALIGN_BOTTOM_MID, -120, -96);
            s_record.play = button(page->root, LV_SYMBOL_PLAY, 56, play_cb);
            if (s_record.play)
            {
                lv_obj_set_style_opa(s_record.play, LV_OPA_40, LV_STATE_DISABLED);
                if (record_audio_cache_bytes() == 0U)
                    lv_obj_add_state(s_record.play, LV_STATE_DISABLED);
                lv_obj_align(s_record.play, LV_ALIGN_BOTTOM_MID, 120, -96);
            }

            // Keep the recorder output volume control consistent with the music page.
            s_record.volume = lv_arc_create(page->root);
            lv_obj_set_size(s_record.volume, 720, 720);
            lv_obj_center(s_record.volume);
            // Restrict pointer hit testing to the circular track so the arc
            // does not intercept taps on the cover or playback controls.
            lv_obj_add_flag(s_record.volume, LV_OBJ_FLAG_ADV_HITTEST);
            lv_arc_set_range(s_record.volume, 0, 100);
            lv_arc_set_bg_angles(s_record.volume, 70, 110);
            lv_arc_set_rotation(s_record.volume, 270);
            // After rotation the start is above the end: reverse makes up louder.
            lv_arc_set_mode(s_record.volume, LV_ARC_MODE_REVERSE);
            uint8_t volume = 72U;
            (void)settings_control_get_audio_volume(&volume);
            lv_arc_set_value(s_record.volume, volume);
            lv_obj_set_style_bg_opa(s_record.volume, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_record.volume, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(s_record.volume, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(s_record.volume, 20, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(s_record.volume, 0, LV_PART_MAIN);
            lv_obj_set_style_arc_color(s_record.volume, lv_color_hex(UI_WHITE), LV_PART_MAIN);
            lv_obj_set_style_arc_width(s_record.volume, 12, LV_PART_MAIN);
            lv_obj_set_style_arc_rounded(s_record.volume, true, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(s_record.volume, LV_OPA_30, LV_PART_MAIN);
            lv_obj_set_style_arc_color(s_record.volume, lv_color_hex(UI_WHITE), LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(s_record.volume, 12, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(s_record.volume, true, LV_PART_INDICATOR);
            lv_obj_set_style_arc_opa(s_record.volume, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(s_record.volume, lv_color_hex(UI_WHITE), LV_PART_KNOB);
            lv_obj_set_style_bg_grad_dir(s_record.volume, LV_GRAD_DIR_NONE, LV_PART_KNOB);
            lv_obj_set_style_bg_opa(s_record.volume, LV_OPA_COVER, LV_PART_KNOB);
            lv_obj_set_style_radius(s_record.volume, LV_RADIUS_CIRCLE, LV_PART_KNOB);
            lv_obj_set_style_pad_all(s_record.volume, 3, LV_PART_KNOB);
            // Keep the icon centers on the track's circle, 8 degrees beyond
            // each endpoint to leave room for the knob, as on the music page.
            constexpr lv_coord_t volume_radius = 720 / 2 - 20 - 12 / 2;
            constexpr int volume_icon_angle = 28;
            const lv_coord_t volume_icon_x = volume_radius * lv_trigo_cos(volume_icon_angle) / (1 << LV_TRIGO_SHIFT);
            const lv_coord_t volume_icon_y = volume_radius * lv_trigo_sin(volume_icon_angle) / (1 << LV_TRIGO_SHIFT);
            s_record.volume_icon = page_ui_label(page->root, LV_SYMBOL_VOLUME_MAX,
                                                 &lv_font_montserrat_16, UI_WHITE);
            lv_obj_align(s_record.volume_icon, LV_ALIGN_CENTER, volume_icon_x, -volume_icon_y);
            s_record.volume_min_icon = page_ui_label(page->root, LV_SYMBOL_MUTE,
                                                     &lv_font_montserrat_16, UI_WHITE);
            lv_obj_align(s_record.volume_min_icon, LV_ALIGN_CENTER, volume_icon_x, volume_icon_y);
            lv_obj_add_event_cb(s_record.volume, volume_changed, LV_EVENT_RELEASED, nullptr);

            // Keep action buttons above the full-screen arc, independently of
            // the arc's advanced hit test implementation.
            lv_obj_move_foreground(s_record.record);
            lv_obj_move_foreground(s_record.pause);
            lv_obj_move_foreground(s_record.play);

            s_record.timer = lv_timer_create(timer, 80, nullptr);
        }
        void enter(Page *)
        {
            // Start RX only for an actual recording; keep the idle amplifier off.
            auto *board = factory::services::board();
            if (board == nullptr || !board->isAudioInitialized() ||
                board->stopMusic() != ESP_OK ||
                board->setAudioOutputEnabled(false) != ESP_OK)
            {
                s_record.action_failed = true;
                status("Microphone unavailable");
            }
        }
        void destroy(Page *)
        {
            (void)record_audio_stop();
            (void)record_audio_stop_cached_playback();
            (void)record_audio_disable_capture();
            if (s_record.timer)
                lv_timer_del(s_record.timer);
            s_record = {};
        }
        void leave(Page *) { (void)ui_page_destroy(PAGE_RECORD); }
    }
    void registerRecordPage()
    {
        Page p{};
        p.id = PAGE_RECORD;
        p.name = "record";
        p.on_create = create;
        p.on_enter = enter;
        p.on_destroy = destroy;
        p.on_leave = leave;
        p.on_gesture = page_ui_back_gesture;
        (void)ui_page_register(&p);
    }
}
extern "C" void record_page_register() { factory::ui::registerRecordPage(); }
