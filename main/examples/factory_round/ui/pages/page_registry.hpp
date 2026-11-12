#pragma once

#ifdef __cplusplus
namespace bsp::t_panel_p4_round
{
class Board;
}

namespace connectivity
{
class Esp32c5Connectivity;
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

void start_page_register(void);
void home_page_register(void);
void music_page_register(void);
void record_page_register(void);
void camera_page_register(void);
void lora_page_register(void);
void file_page_register(void);
void settings_page_register(void);

#ifdef __cplusplus
}

namespace factory::ui
{
/** Provide the round-board power source used by the Home status footer. */
void setHomeBoard(bsp::t_panel_p4_round::Board *board);
/** Provide the round-board camera source used by the Camera page. */
void setCameraBoard(bsp::t_panel_p4_round::Board *board);

/** Provide the ESP32-C5 status source used by the Home connectivity footer. */
void setHomeConnectivity(connectivity::Esp32c5Connectivity *connectivity);
}
#endif
