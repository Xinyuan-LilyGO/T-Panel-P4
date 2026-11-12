#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_CONTROL_MAX_PAYLOAD 255

typedef struct
{
    float frequency_mhz;
    float bandwidth_khz;
    uint8_t spreading_factor;
    uint8_t coding_rate;
    int8_t output_power_dbm;
    uint16_t preamble_length;
    bool crc_enabled;
} lora_control_config_t;

typedef enum
{
    LORA_CONTROL_EVENT_TX_DONE = 0,
    LORA_CONTROL_EVENT_RX_PACKET,
    LORA_CONTROL_EVENT_CONFIG_APPLIED,
    LORA_CONTROL_EVENT_ERROR,
} lora_control_event_type_t;

typedef struct
{
    lora_control_event_type_t type;
    int32_t code;
    float rssi_dbm;
    float snr_db;
    char payload[LORA_CONTROL_MAX_PAYLOAD + 1];
    lora_control_config_t config;
} lora_control_event_t;

bool lora_control_is_ready(void);
void lora_control_initialize(void);
void lora_control_deinitialize(void);
bool lora_control_set_idle(void);
bool lora_control_send(const char *payload);
bool lora_control_start_timed(uint32_t interval_ms);
bool lora_control_start_receive(void);
bool lora_control_apply_config(const lora_control_config_t *config);
bool lora_control_poll_event(lora_control_event_t *event);

#ifdef __cplusplus
}
#endif
