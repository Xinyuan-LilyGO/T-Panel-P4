#include "lora_control.h"
#include "service_context.hpp"

#include <bsp/t_panel_p4_standard.hpp>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
constexpr char kTag[] = "factory_lora";
constexpr UBaseType_t kCommandQueueLength = 8;
constexpr UBaseType_t kEventQueueLength = 16;
constexpr uint32_t kWorkerPollMs = 20;

enum class CommandType : uint8_t
{
    idle,
    send,
    timed,
    receive,
    apply_config,
};

enum class WorkerMode : uint8_t
{
    idle,
    timed,
    receive,
};

struct Command
{
    CommandType type{};
    uint32_t interval_ms{};
    char payload[LORA_CONTROL_MAX_PAYLOAD + 1]{};
    lora_control_config_t config{};
};

#define s_board (factory::services::board())
QueueHandle_t s_commands{};
QueueHandle_t s_events{};
TaskHandle_t s_task{};

void pushEvent(const lora_control_event_t &event)
{
    if (s_events == nullptr)
        return;
    if (xQueueSend(s_events, &event, 0) == pdTRUE)
        return;
    lora_control_event_t discarded{};
    (void)xQueueReceive(s_events, &discarded, 0);
    (void)xQueueSend(s_events, &event, 0);
}

void pushError(int32_t code, const char *operation)
{
    lora_control_event_t event{};
    event.type = LORA_CONTROL_EVENT_ERROR;
    event.code = code;
    if (operation != nullptr)
        std::snprintf(event.payload, sizeof(event.payload), "%s", operation);
    pushEvent(event);
    ESP_LOGE(kTag, "%s failed: %ld", operation != nullptr ? operation : "operation",
             static_cast<long>(code));
}

void IRAM_ATTR packetReceivedIsr()
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_task != nullptr)
        vTaskNotifyGiveFromISR(s_task, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE)
        portYIELD_FROM_ISR();
}

bool startReceive(LR2021 *radio)
{
    if (radio == nullptr)
        return false;
    radio->setPacketReceivedAction(packetReceivedIsr);
    const int16_t state = radio->startReceive();
    if (state == RADIOLIB_ERR_NONE)
        return true;
    radio->clearPacketReceivedAction();
    pushError(state, "receive start");
    return false;
}

void stopRadio(LR2021 *radio)
{
    if (radio == nullptr)
        return;
    radio->clearPacketReceivedAction();
    const int16_t state = radio->standby();
    if (state != RADIOLIB_ERR_NONE)
        pushError(state, "standby");
}

void transmit(LR2021 *radio, const char *payload, WorkerMode resume_mode)
{
    if (radio == nullptr || payload == nullptr || payload[0] == '\0')
    {
        pushError(ESP_ERR_INVALID_ARG, "transmit");
        return;
    }

    radio->clearPacketReceivedAction();
    const int16_t state = radio->transmit(
        reinterpret_cast<const uint8_t *>(payload), std::strlen(payload));
    if (state == RADIOLIB_ERR_NONE)
    {
        lora_control_event_t event{};
        event.type = LORA_CONTROL_EVENT_TX_DONE;
        std::snprintf(event.payload, sizeof(event.payload), "%s", payload);
        pushEvent(event);
    }
    else
    {
        pushError(state, "transmit");
    }

    if (resume_mode == WorkerMode::receive)
        (void)startReceive(radio);
}

bsp::t_panel_p4_standard::LoraConfig toBspConfig(
    const lora_control_config_t &config)
{
    bsp::t_panel_p4_standard::LoraConfig result{};
    result.frequency_mhz = config.frequency_mhz;
    result.bandwidth_khz = config.bandwidth_khz;
    result.spreading_factor = config.spreading_factor;
    result.coding_rate = config.coding_rate;
    result.output_power_dbm = config.output_power_dbm;
    result.preamble_length = config.preamble_length;
    result.crc_enabled = config.crc_enabled;
    return result;
}

lora_control_config_t fromBspConfig(
    const bsp::t_panel_p4_standard::LoraConfig &config)
{
    return {
        .frequency_mhz = config.frequency_mhz,
        .bandwidth_khz = config.bandwidth_khz,
        .spreading_factor = config.spreading_factor,
        .coding_rate = config.coding_rate,
        .output_power_dbm = config.output_power_dbm,
        .preamble_length = config.preamble_length,
        .crc_enabled = config.crc_enabled,
    };
}

bool applyConfig(const lora_control_config_t &requested, WorkerMode mode)
{
    if (s_board == nullptr)
        return false;

    bsp::t_panel_p4_standard::LoraConfig previous{};
    if (const auto *current = s_board->loraConfig(); current != nullptr)
        previous = *current;

    if (LR2021 *radio = s_board->loraRadio(); radio != nullptr)
        stopRadio(radio);
    esp_err_t result = s_board->deinitializeLora();
    if (result == ESP_OK)
        result = s_board->initializeLora(toBspConfig(requested));

    if (result != ESP_OK)
    {
        ESP_LOGE(kTag, "configuration failed, restoring previous settings: %s",
                 esp_err_to_name(result));
        (void)s_board->deinitializeLora();
        (void)s_board->initializeLora(previous);
        pushError(result, "configuration");
        if (mode == WorkerMode::receive)
            (void)startReceive(s_board->loraRadio());
        return false;
    }

    lora_control_event_t event{};
    event.type = LORA_CONTROL_EVENT_CONFIG_APPLIED;
    event.config = requested;
    pushEvent(event);
    if (mode == WorkerMode::receive)
        (void)startReceive(s_board->loraRadio());
    return true;
}

void handleReceivedPacket(LR2021 *radio)
{
    if (radio == nullptr)
        return;

    const size_t packet_length = std::min(
        radio->getPacketLength(), static_cast<size_t>(LORA_CONTROL_MAX_PAYLOAD));
    lora_control_event_t event{};
    event.type = LORA_CONTROL_EVENT_RX_PACKET;
    const int16_t state = radio->readData(
        reinterpret_cast<uint8_t *>(event.payload), packet_length);
    event.payload[packet_length] = '\0';
    if (state == RADIOLIB_ERR_NONE)
    {
        event.rssi_dbm = radio->getRSSI();
        event.snr_db = radio->getSNR();
        if (s_board != nullptr)
        {
            if (const auto *config = s_board->loraConfig(); config != nullptr)
                event.config = fromBspConfig(*config);
        }
        pushEvent(event);
    }
    else
    {
        pushError(state, "receive data");
    }
    (void)startReceive(radio);
}

void workerTask(void *)
{
    WorkerMode mode = WorkerMode::idle;
    char timed_payload[LORA_CONTROL_MAX_PAYLOAD + 1]{};
    uint32_t timed_sequence = 1;
    uint32_t timed_interval_ms = 1000;
    TickType_t next_transmit = 0;

    while (true)
    {
        Command command{};
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(kWorkerPollMs)) == pdTRUE)
        {
            LR2021 *radio = s_board != nullptr ? s_board->loraRadio() : nullptr;
            switch (command.type)
            {
            case CommandType::idle:
                mode = WorkerMode::idle;
                stopRadio(radio);
                break;
            case CommandType::send:
                mode = WorkerMode::idle;
                stopRadio(radio);
                transmit(radio, command.payload, mode);
                break;
            case CommandType::timed:
                mode = WorkerMode::timed;
                stopRadio(radio);
                timed_sequence = 1;
                timed_interval_ms = command.interval_ms;
                next_transmit = xTaskGetTickCount();
                break;
            case CommandType::receive:
                mode = WorkerMode::receive;
                stopRadio(radio);
                if (!startReceive(radio))
                    mode = WorkerMode::idle;
                break;
            case CommandType::apply_config:
                (void)applyConfig(command.config, mode);
                break;
            }
        }

        LR2021 *radio = s_board != nullptr ? s_board->loraRadio() : nullptr;
        if (mode == WorkerMode::receive && ulTaskNotifyTake(pdTRUE, 0) > 0U)
            handleReceivedPacket(radio);

        if (mode == WorkerMode::timed)
        {
            const TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(now - next_transmit) >= 0)
            {
                std::snprintf(timed_payload, sizeof(timed_payload),
                              "T-Panel-P4 LR2021 Test #%lu",
                              static_cast<unsigned long>(timed_sequence));
                transmit(radio, timed_payload, mode);
                ++timed_sequence;
                if (timed_sequence == 0U)
                    timed_sequence = 1U;
                next_transmit = now + pdMS_TO_TICKS(timed_interval_ms);
            }
        }
    }
}

bool enqueue(Command &command)
{
    return s_commands != nullptr && xQueueSend(s_commands, &command, 0) == pdTRUE;
}
}

extern "C" void lora_control_initialize(void)
{
    if (s_commands == nullptr)
        s_commands = xQueueCreate(kCommandQueueLength, sizeof(Command));
    if (s_events == nullptr)
        s_events = xQueueCreate(kEventQueueLength, sizeof(lora_control_event_t));
    if (s_task == nullptr && s_commands != nullptr && s_events != nullptr)
        (void)xTaskCreate(workerTask, "lora_control", 4096, nullptr, 5, &s_task);
}

extern "C" void lora_control_deinitialize(void)
{
    if (s_task != nullptr)
    {
        vTaskDelete(s_task);
        s_task = nullptr;
    }
    if (s_commands != nullptr)
    {
        vQueueDelete(s_commands);
        s_commands = nullptr;
    }
    if (s_events != nullptr)
    {
        vQueueDelete(s_events);
        s_events = nullptr;
    }
}

extern "C" bool lora_control_is_ready(void)
{
    return s_board != nullptr && s_board->isLoraInitialized() && s_task != nullptr;
}

extern "C" bool lora_control_set_idle(void)
{
    Command command{};
    command.type = CommandType::idle;
    return enqueue(command);
}

extern "C" bool lora_control_send(const char *payload)
{
    if (payload == nullptr || payload[0] == '\0')
        return false;
    Command command{};
    command.type = CommandType::send;
    std::snprintf(command.payload, sizeof(command.payload), "%s", payload);
    return enqueue(command);
}

extern "C" bool lora_control_start_timed(uint32_t interval_ms)
{
    if (interval_ms == 0U)
        return false;
    Command command{};
    command.type = CommandType::timed;
    command.interval_ms = interval_ms;
    return enqueue(command);
}

extern "C" bool lora_control_start_receive(void)
{
    Command command{};
    command.type = CommandType::receive;
    return enqueue(command);
}

extern "C" bool lora_control_apply_config(const lora_control_config_t *config)
{
    if (config == nullptr)
        return false;
    Command command{};
    command.type = CommandType::apply_config;
    command.config = *config;
    return enqueue(command);
}

extern "C" bool lora_control_poll_event(lora_control_event_t *event)
{
    return event != nullptr && s_events != nullptr &&
           xQueueReceive(s_events, event, 0) == pdTRUE;
}
