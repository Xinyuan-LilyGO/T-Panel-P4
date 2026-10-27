#include "ui.h"

#include <ui/core/status_bar.h>
#include <ui/pages/page_registry.h>

extern "C" esp_err_t factory_ui_start(void)
{
    start_page_register();
    home_page_register();
    music_page_register();
    record_page_register();
    camera_page_register();
    lora_page_register();
    file_page_register();
    settings_page_register();
    if (ui_init() != RT_EOK)
        return ESP_FAIL;
    return ESP_OK;
}
