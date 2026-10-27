#pragma once

#include <bsp/t_panel_p4_standard.hpp>
#include <esp32c5_connectivity/esp32c5_connectivity.hpp>

namespace factory::services
{
struct Context
{
    bsp::t_panel_p4_standard::Board *board{};
    connectivity::Esp32c5Connectivity *connectivity{};
    connectivity::Esp32c5WifiStationConfig *wifi_config{};
    bool *wifi_config_valid{};
    esp_err_t *connectivity_result{};
};

void set_context(bsp::t_panel_p4_standard::Board &board,
                 connectivity::Esp32c5Connectivity &connectivity,
                 connectivity::Esp32c5WifiStationConfig *wifi_config = nullptr,
                 bool *wifi_config_valid = nullptr,
                 esp_err_t *connectivity_result = nullptr);
const Context &context();
bsp::t_panel_p4_standard::Board *board();
connectivity::Esp32c5Connectivity *connectivity();
}
