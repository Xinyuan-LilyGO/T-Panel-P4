#include "page_common.hpp"
#include "page_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if defined(CONFIG_BT_CONTROLLER_ENABLED) && CONFIG_BT_CONTROLLER_ENABLED
#include <esp_bt.h>
#endif
#include <lvgl_page_manager.h>
#include <pdl/drivers/charge/axp517/axp517.hpp>

#include <bsp/board.hpp>
#include <esp32c5_connectivity/esp32c5_connectivity.hpp>

namespace factory::ui
{
    namespace
    {
        constexpr char kTag[] = "factory_round_home";
        constexpr std::size_t kAppCount = 6;
        constexpr std::uint32_t kHomeTimerPeriodMs = 1000;
        constexpr lv_coord_t kAppCardWidth = 80;
        constexpr lv_coord_t kAppCardHeight = 110;
        constexpr lv_coord_t kAppIconSize = 80;
        constexpr lv_coord_t kAppColumnGap = 40;
        constexpr lv_coord_t kAppRowGap = 24;
        constexpr lv_coord_t kAppGridWidth =
            kAppCardWidth * 3 + kAppColumnGap * 2;
        constexpr lv_coord_t kAppGridHeight =
            kAppCardHeight * 2 + kAppRowGap;
        lv_obj_t *s_status_time_label{};
        lv_obj_t *s_status_date_label{};
        lv_obj_t *s_status_wifi_label{};
        lv_obj_t *s_status_bluetooth_label{};
        lv_obj_t *s_status_battery_icon_label{};
        lv_obj_t *s_status_battery_percent_label{};
        bsp::t_panel_p4_round::Board *s_home_board{};
        connectivity::Esp32c5Connectivity *s_home_connectivity{};
        bool s_time_sync_started{};

        void timeSyncTask(void *)
        {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_err_t result = esp_netif_sntp_init(&config);
            if (result == ESP_OK)
            {
                result = ESP_ERR_TIMEOUT;
                for (int attempt = 0; attempt < 10; ++attempt)
                {
                    result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
                    if (result == ESP_OK ||
                        (result != ESP_ERR_TIMEOUT && result != ESP_ERR_NOT_FINISHED))
                        break;
                }
            }
            if (result == ESP_OK)
            {
                setenv("TZ", "CST-8", 1);
                tzset();
                ESP_LOGI(kTag, "SNTP time synchronized");
            }
            else
            {
                ESP_LOGW(kTag, "SNTP synchronization failed: %s", esp_err_to_name(result));
            }
            esp_netif_sntp_deinit();
            s_time_sync_started = false;
            vTaskDelete(nullptr);
        }

        typedef struct
        {
            const char *name;
            const char *icon;
            PageId page;
        } home_app_t;

        static const home_app_t s_home_apps[] = {
            {"Music", LV_SYMBOL_AUDIO, PageId::PAGE_MUSIC},
            {"Record", LV_SYMBOL_DRIVE, PageId::PAGE_RECORD},
            {"Camera", LV_SYMBOL_IMAGE, PageId::PAGE_CAMERA},
            {"Lora", LV_SYMBOL_WIFI, PageId::PAGE_LORA},
            {"File", LV_SYMBOL_FILE, PageId::PAGE_FILE},
            {"Set", LV_SYMBOL_SETTINGS, PageId::PAGE_SET},
        };

        void updateStatusTime()
        {
            if (s_status_time_label == nullptr || s_status_date_label == nullptr)
                return;

            std::time_t now = std::time(nullptr);
            std::tm local_time{};
            localtime_r(&now, &local_time);

            char time_text[16]{};
            char date_text[64]{};
            std::strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
            std::strftime(date_text, sizeof(date_text), "%A, %B %d, %Y", &local_time);
            lv_label_set_text(s_status_time_label, time_text);
            lv_label_set_text(s_status_date_label, date_text);
        }

        void updateStatusIndicators()
        {
            if (s_status_wifi_label != nullptr || s_status_bluetooth_label != nullptr)
            {
                bool wifi_connected = false;
                bool bluetooth_enabled = false;
                if (s_home_connectivity != nullptr)
                {
                    const connectivity::Esp32c5ConnectivityStatus status =
                        s_home_connectivity->status();
                    wifi_connected = status.wifi_connected;
                    bluetooth_enabled = status.bluetooth_controller_enabled;
                    if (wifi_connected && !s_time_sync_started)
                    {
                        s_time_sync_started = true;
                        if (xTaskCreate(timeSyncTask, "home_sntp", 3072, nullptr, 3, nullptr) != pdPASS)
                            s_time_sync_started = false;
                    }
                }

                if (s_status_wifi_label != nullptr)
                {
                    lv_obj_set_style_text_color(
                        s_status_wifi_label,
                        lv_color_hex(wifi_connected ? UI_WIHTE : UI_GTAY), LV_PART_MAIN);
                }
                if (s_status_bluetooth_label != nullptr)
                {
                    lv_obj_set_style_text_color(
                        s_status_bluetooth_label,
                        lv_color_hex(bluetooth_enabled ? UI_WIHTE : UI_GTAY), LV_PART_MAIN);
                }
            }

            if (s_status_battery_icon_label == nullptr ||
                s_status_battery_percent_label == nullptr)
                return;

            std::uint8_t battery_percent{};
            pdl::charge::Axp517PowerStatus power_status{};
            const bool battery_valid =
                s_home_board != nullptr &&
                s_home_board->readBatteryPercentage(battery_percent) == ESP_OK &&
                s_home_board->readPowerStatus(power_status) == ESP_OK;
            if (!battery_valid)
            {
                lv_label_set_text(s_status_battery_icon_label, LV_SYMBOL_BATTERY_EMPTY);
                lv_label_set_text(s_status_battery_percent_label, "--%");
                return;
            }

            const char *battery_icon = LV_SYMBOL_BATTERY_EMPTY;
            if (pdl::charge::isBatteryCharging(power_status))
                battery_icon = LV_SYMBOL_CHARGE;
            else if (battery_percent >= 90U)
                battery_icon = LV_SYMBOL_BATTERY_FULL;
            else if (battery_percent >= 50U)
                battery_icon = LV_SYMBOL_BATTERY_FULL;

            lv_label_set_text(s_status_battery_icon_label, battery_icon);
            lv_label_set_text_fmt(
                s_status_battery_percent_label, "%u%%",
                static_cast<unsigned>(battery_percent));
            lv_obj_set_style_text_color(
                s_status_battery_icon_label,
                lv_color_hex(battery_percent <= 15U ? 0xe235ac : UI_WIHTE),
                LV_PART_MAIN);
            lv_obj_set_style_text_color(
                s_status_battery_percent_label,
                lv_color_hex(battery_percent <= 15U ? 0xe235ac : UI_WIHTE),
                LV_PART_MAIN);
        }

        void homePageTimer(Page *)
        {
            updateStatusTime();
            updateStatusIndicators();
        }

        void appCardClicked(lv_event_t *event)
        {
            const auto index = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(
                lv_obj_get_user_data(lv_event_get_current_target_obj(event))));
            if (index >= kAppCount)
                return;

            const rt_err_t result = ui_page_switch_async(
                static_cast<PageType>(s_home_apps[index].page));
            if (result != RT_EOK)
            {
                ESP_LOGE(
                    kTag, "failed to switch to %s (page=%d, err=%d)",
                    s_home_apps[index].name,
                    static_cast<int>(s_home_apps[index].page),
                    static_cast<int>(result));
            }
        }

        void createAppCard(lv_obj_t *parent, std::size_t index)
        {
            if (parent == nullptr || index >= kAppCount)
                return;

            const lv_coord_t column = static_cast<lv_coord_t>(index % 3);
            const lv_coord_t row = static_cast<lv_coord_t>(index / 3);
            const lv_coord_t x = column * (kAppCardWidth + kAppColumnGap);
            const lv_coord_t y = row * (kAppCardHeight + kAppRowGap);

            lv_obj_t *card = page_ui_container(parent, kAppCardWidth, kAppCardHeight);
            if (card == nullptr)
                return;
            lv_obj_set_pos(card, x, y);
            lv_obj_set_style_bg_opa(card, LV_OPA_0, LV_PART_MAIN);
            lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(
                card, reinterpret_cast<void *>(static_cast<std::uintptr_t>(index)));
            lv_obj_add_event_cb(card, appCardClicked, LV_EVENT_CLICKED, nullptr);

            lv_obj_t *icon = lv_obj_create(card);
            if (icon != nullptr)
            {
                lv_obj_set_size(icon, kAppIconSize - 8, kAppIconSize - 8);
                lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
                lv_obj_set_style_border_width(icon, 1, LV_STATE_PRESSED);
                lv_obj_set_style_border_color(icon, lv_color_hex(UI_WIHTE), LV_STATE_PRESSED);
                lv_obj_set_style_border_opa(icon, LV_OPA_COVER, LV_STATE_PRESSED);
                lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, LV_STATE_PRESSED);
                page_ui_apply_icon_style(icon);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_EVENT_BUBBLE);

                lv_obj_t *icon_label = lv_label_create(icon);
                if (icon_label != nullptr)
                {
                    lv_label_set_text(icon_label, s_home_apps[index].icon);
                    lv_obj_set_style_text_font(
                        icon_label, &lv_font_montserrat_24, LV_PART_MAIN);
                    lv_obj_set_style_text_color(
                        icon_label, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                    lv_obj_center(icon_label);
                    lv_obj_remove_flag(icon_label, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_add_flag(icon_label, LV_OBJ_FLAG_EVENT_BUBBLE);
                }
            }

            lv_obj_t *name = lv_label_create(card);
            if (name != nullptr)
            {
                lv_obj_set_size(name, kAppCardWidth, 18);
                lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, 0);
                lv_label_set_text(name, s_home_apps[index].name);
                lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_set_style_text_color(name, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
                lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(name, LV_OBJ_FLAG_EVENT_BUBBLE);
            }
        }

        void destroyHomePage()
        {
        }

        void homePageCreate(Page *page)
        {
            if (page == nullptr)
                return;
            ui_create_background(page->root);
            lv_obj_t *home_label = page_ui_label(page->root, "Home", &lv_font_montserrat_20, UI_GTAY);
            lv_obj_align(home_label, LV_ALIGN_TOP_MID, 0, 60);

            lv_obj_t *status_cont = page_ui_container(page->root, 416, 84);
            if (status_cont == nullptr)
                return;
            lv_obj_align(status_cont, LV_ALIGN_TOP_MID, 0, 100);
            page_ui_apply_icon_style(status_cont);
            lv_obj_set_style_radius(status_cont, 28, LV_PART_MAIN);

            s_status_time_label = lv_label_create(status_cont);
            if (s_status_time_label != nullptr)
            {
                lv_obj_set_pos(s_status_time_label, 16, 7);
                lv_obj_set_style_text_font(
                    s_status_time_label, &lv_font_montserrat_24, LV_PART_MAIN);
                lv_obj_set_style_text_color(
                    s_status_time_label, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
            }

            s_status_date_label = lv_label_create(status_cont);
            if (s_status_date_label != nullptr)
            {
                lv_obj_set_pos(s_status_date_label, 16, 48);
                lv_obj_set_style_text_font(
                    s_status_date_label, &lv_font_montserrat_14, LV_PART_MAIN);
                lv_obj_set_style_text_color(
                    s_status_date_label, lv_color_hex(UI_WIHTE), LV_PART_MAIN);
                lv_obj_set_style_text_opa(s_status_date_label, LV_OPA_80, LV_PART_MAIN);
            }

            s_status_wifi_label = lv_label_create(status_cont);
            if (s_status_wifi_label != nullptr)
            {
                lv_label_set_text(s_status_wifi_label, LV_SYMBOL_WIFI);
                lv_obj_set_size(s_status_wifi_label, 32, 28);
                lv_obj_set_pos(s_status_wifi_label, 248, 26);
                lv_obj_set_style_text_align(s_status_wifi_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_set_style_text_font(s_status_wifi_label, &lv_font_montserrat_20, LV_PART_MAIN);
                lv_obj_set_style_text_color(s_status_wifi_label, lv_color_hex(UI_GTAY), LV_PART_MAIN);
            }

            s_status_bluetooth_label = lv_label_create(status_cont);
            if (s_status_bluetooth_label != nullptr)
            {
                lv_label_set_text(s_status_bluetooth_label, LV_SYMBOL_BLUETOOTH);
                lv_obj_set_size(s_status_bluetooth_label, 32, 28);
                lv_obj_set_pos(s_status_bluetooth_label, 288, 26);
                lv_obj_set_style_text_align(s_status_bluetooth_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_set_style_text_font(s_status_bluetooth_label, &lv_font_montserrat_20, LV_PART_MAIN);
                lv_obj_set_style_text_color(s_status_bluetooth_label, lv_color_hex(UI_GTAY), LV_PART_MAIN);
            }

            s_status_battery_icon_label = lv_label_create(status_cont);
            if (s_status_battery_icon_label != nullptr)
            {
                lv_obj_set_size(s_status_battery_icon_label, 32, 28);
                lv_obj_set_pos(s_status_battery_icon_label, 328, 26);
                lv_obj_set_style_text_align(s_status_battery_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_set_style_text_font(s_status_battery_icon_label, &lv_font_montserrat_20, LV_PART_MAIN);
                lv_obj_set_style_text_color(s_status_battery_icon_label, lv_color_hex(UI_GTAY), LV_PART_MAIN);
            }

            s_status_battery_percent_label = lv_label_create(status_cont);
            if (s_status_battery_percent_label != nullptr)
            {
                lv_obj_set_size(s_status_battery_percent_label, 48, 24);
                lv_obj_set_pos(s_status_battery_percent_label, 360, 30);
                lv_obj_set_style_text_align(s_status_battery_percent_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_set_style_text_font(s_status_battery_percent_label, &lv_font_montserrat_16, LV_PART_MAIN);
                lv_obj_set_style_text_color(s_status_battery_percent_label, lv_color_hex(UI_GTAY), LV_PART_MAIN);
            }
            updateStatusTime();
            updateStatusIndicators();

            lv_obj_t *app_cont = page_ui_container(page->root, kAppGridWidth, kAppGridHeight);
            if (app_cont == nullptr)
                return;
            lv_obj_align(app_cont, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_bg_opa(app_cont, LV_OPA_0, LV_PART_MAIN);
            lv_obj_set_style_border_opa(app_cont, LV_OPA_0, LV_PART_MAIN);
            for (std::size_t index = 0; index < kAppCount; ++index)
                createAppCard(app_cont, index);
            lv_obj_t *board_label = page_ui_label(page->root, "T-PANEL-P4 ROUND", &lv_font_montserrat_20, UI_GTAY);
            lv_obj_align(board_label, LV_ALIGN_BOTTOM_MID, 0, -80);
        }

        void homePageDestroy(Page *)
        {
            s_status_time_label = nullptr;
            s_status_date_label = nullptr;
            s_status_wifi_label = nullptr;
            s_status_bluetooth_label = nullptr;
            s_status_battery_icon_label = nullptr;
            s_status_battery_percent_label = nullptr;
            destroyHomePage();
        }

        void homePageLeave(Page *)
        {
            // The manager changes current_page before calling on_leave, so the page
            // can be destroyed here without hitting the manager's busy-page guard.
            (void)ui_page_destroy(PAGE_HOME);
        }

    } // namespace

    void setHomeBoard(bsp::t_panel_p4_round::Board *board)
    {
        s_home_board = board;
    }

    void setHomeConnectivity(connectivity::Esp32c5Connectivity *connectivity)
    {
        s_home_connectivity = connectivity;
    }

    void registerHomePage()
    {
        Page page{};
        page.id = PAGE_HOME;
        page.name = "home";
        page.on_create = homePageCreate;
        page.on_destroy = homePageDestroy;
        page.on_leave = homePageLeave;
        page.page_timer = homePageTimer;
        page.timer_interval_ms = kHomeTimerPeriodMs;
        (void)ui_page_register(&page);
    }

} // namespace factory::ui

extern "C" void home_page_register(void)
{
    factory::ui::registerHomePage();
}
