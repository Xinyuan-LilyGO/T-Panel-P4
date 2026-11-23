#pragma once

#include <bsp/api/board.hpp>
#include <bsp/api/slave_c5.hpp>

namespace factory::services
{
struct Context
{
    bsp::t_panel_p4_round::Board *board{};
    bsp::Esp32c5Slave *connectivity{};
    bsp::Esp32c5WifiStationConfig *wifi_config{};
    bool *wifi_config_valid{};
    esp_err_t *connectivity_result{};
};

void set_context(
    bsp::t_panel_p4_round::Board &board,
    bsp::Esp32c5Slave &connectivity,
    bsp::Esp32c5WifiStationConfig *wifi_config = nullptr,
    bool *wifi_config_valid = nullptr,
    esp_err_t *connectivity_result = nullptr);

const Context &context();
bsp::t_panel_p4_round::Board *board();
bsp::Esp32c5Slave *connectivity();
}
