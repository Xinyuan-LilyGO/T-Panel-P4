#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool camera_control_enter(void);
bool camera_control_leave(void);
const char *camera_control_last_error_name(void);
bool camera_control_read_frame_rgb565(uint16_t *destination,
                                      size_t destination_pixels,
                                      uint16_t destination_width,
                                      uint16_t destination_height);
bool camera_control_get_mode(uint16_t *width, uint16_t *height, uint8_t *frames_per_second);
bool camera_control_capture_photo(char *path, size_t path_size);
int camera_control_capture_status(char *path, size_t path_size);
bool camera_control_delete_photo(const char *path);

#ifdef __cplusplus
}
#endif
