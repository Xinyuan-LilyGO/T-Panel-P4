#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool record_audio_read_db(float *left_db, float *right_db);
bool record_audio_enable_capture(void);
bool record_audio_disable_capture(void);
bool record_audio_is_recording(void);
bool record_audio_is_paused(void);
bool record_audio_pause(bool paused);
uint32_t record_audio_duration_ms(void);
size_t record_audio_cache_bytes(void);
uint8_t record_audio_level_percent(void);
void record_audio_get_waveform(uint8_t *levels, size_t count);
bool record_audio_start(void);
bool record_audio_stop(void);
bool record_audio_save(void);
bool record_audio_play_cached(void);
bool record_audio_stop_cached_playback(void);
bool record_audio_is_playing(void);
void record_audio_clear_cache(void);
bool record_audio_latest_path(char *path, size_t size);
bool record_audio_wav_duration(const char *path, uint32_t *duration_ms);

#ifdef __cplusplus
}
#endif
