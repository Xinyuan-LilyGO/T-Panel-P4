#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    MUSIC_AUDIO_STATE_IDLE = 0,
    MUSIC_AUDIO_STATE_STARTING,
    MUSIC_AUDIO_STATE_PLAYING,
    MUSIC_AUDIO_STATE_PAUSED,
    MUSIC_AUDIO_STATE_STOPPED,
    MUSIC_AUDIO_STATE_FINISHED,
    MUSIC_AUDIO_STATE_ERROR,
} music_audio_state_t;

typedef struct
{
    const char *codec;
    int32_t sample_rate_hz;
    int32_t bitrate_bps;
    uint8_t channels;
    uint8_t bits_per_sample;
} music_audio_format_t;

bool music_audio_enter(void);
bool music_audio_leave(void);
bool music_audio_play(const char *path);
const char *music_audio_last_error_name(void);
void music_audio_pause(bool paused);
void music_audio_stop(void);
bool music_audio_is_playing(void);
bool music_audio_is_paused(void);
music_audio_state_t music_audio_state(void);
void music_audio_get_format(music_audio_format_t *format);
uint32_t music_audio_position_ms(void);
uint32_t music_audio_duration_ms(void);
uint8_t music_audio_level_percent(void);
bool music_audio_current_path(char *path, size_t size);
void music_audio_get_waveform(int16_t *samples, size_t count);
bool music_audio_probe_duration(const char *path, uint32_t *duration_ms);
bool music_audio_seek_ms(uint32_t position_ms);
void music_audio_set_volume_percent(int32_t percent);
uint8_t music_audio_get_volume_percent(void);
bool music_audio_load_metadata(const char *path,
                               uint8_t **cover_data, size_t *cover_size,
                               char *lyrics, size_t lyrics_size);
void music_audio_free_cover(uint8_t *cover_data);
bool music_audio_read_metadata(const char *path,
                               char *title, size_t title_size,
                               char *artist, size_t artist_size,
                               char *album, size_t album_size);

#ifdef __cplusplus
}
#endif
