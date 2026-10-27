#include "bluetooth_control.h"

#include "service_context.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_hosted.h>

#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

namespace
{
constexpr char kAdvertisedName[] = "T-PANEL-P4";
constexpr char kUnavailable[] = "UNAVAILABLE";
constexpr char kCharacteristicValue[] = "T-PANEL-P4 BLE peripheral";
constexpr char kTag[] = "bluetooth_control";

/* Keep these UUIDs stable so phone applications can save the service across
 * firmware updates. */
static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x7b, 0x2e, 0x4a, 0x1d, 0x91, 0x6d, 0x4f, 0x1d,
    0x8f, 0x8b, 0x6e, 0xa2, 0x5b, 0x8a, 0x00, 0x01);
static const ble_uuid128_t s_characteristic_uuid = BLE_UUID128_INIT(
    0x7b, 0x2e, 0x4a, 0x1d, 0x91, 0x6d, 0x4f, 0x1d,
    0x8f, 0x8b, 0x6e, 0xa2, 0x5b, 0x8a, 0x00, 0x02);

std::atomic<bool> s_host_initialized{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_advertising{false};
std::atomic<std::uint16_t> s_connection_handle{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> s_rssi_valid{false};
std::atomic<int> s_rssi_dbm{0};
std::atomic<bool> s_peer_valid{false};
std::atomic<std::uint64_t> s_peer_address{0};
std::uint8_t s_own_addr_type{};
std::uint16_t s_characteristic_value_handle{};

connectivity::Esp32c5Connectivity *radio()
{
    return factory::services::connectivity();
}

int gatt_access(uint16_t, uint16_t attr_handle,
                ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR ||
        attr_handle != s_characteristic_value_handle)
        return BLE_ATT_ERR_UNLIKELY;

    return os_mbuf_append(ctxt->om, kCharacteristicValue,
                          sizeof(kCharacteristicValue) - 1U) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static ble_gatt_chr_def s_characteristics[] = {
    {&s_characteristic_uuid.u, gatt_access, nullptr, nullptr,
     BLE_GATT_CHR_F_READ, 0, &s_characteristic_value_handle, nullptr},
    {nullptr, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr},
};

static ble_gatt_svc_def s_services[] = {
    {BLE_GATT_SVC_TYPE_PRIMARY, &s_service_uuid.u, nullptr, s_characteristics},
    {0, nullptr, nullptr, nullptr},
};

void start_advertising();

void remember_peer(std::uint16_t connection_handle)
{
    ble_gap_conn_desc desc{};
    const int rc = ble_gap_conn_find(connection_handle, &desc);
    if (rc != 0)
    {
        ESP_LOGW(kTag, "Could not read connected central address, rc=%d", rc);
        s_peer_valid.store(false);
        return;
    }

    std::uint64_t packed_address = 0;
    for (std::size_t index = 0; index < 6U; ++index)
        packed_address |= static_cast<std::uint64_t>(desc.peer_ota_addr.val[index])
                          << (index * 8U);
    s_peer_address.store(packed_address);
    s_peer_valid.store(true);
    ESP_LOGI(kTag, "BLE central address type=%u addr=%02X:%02X:%02X:%02X:%02X:%02X",
             static_cast<unsigned>(desc.peer_ota_addr.type),
             desc.peer_ota_addr.val[5], desc.peer_ota_addr.val[4],
             desc.peer_ota_addr.val[3], desc.peer_ota_addr.val[2],
             desc.peer_ota_addr.val[1], desc.peer_ota_addr.val[0]);
}

void clear_peer()
{
    s_peer_valid.store(false);
    s_peer_address.store(0);
}

int gap_event(ble_gap_event *event, void *)
{
    if (event == nullptr)
        return 0;

    switch (event->type)
    {
#if defined(BLE_GAP_EVENT_LINK_ESTAB)
    case BLE_GAP_EVENT_LINK_ESTAB:
#else
    case BLE_GAP_EVENT_CONNECT:
#endif
        if (event->connect.status == 0)
        {
            ESP_LOGI(kTag, "BLE central connected, handle=%u",
                     static_cast<unsigned>(event->connect.conn_handle));
            s_connected.store(true);
            s_advertising.store(false);
            s_connection_handle.store(event->connect.conn_handle);
            s_rssi_valid.store(false);
            remember_peer(event->connect.conn_handle);
        }
        else
        {
            ESP_LOGW(kTag, "BLE connection attempt failed, status=%d",
                     event->connect.status);
            s_advertising.store(false);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(kTag, "BLE central disconnected, reason=%d",
                 event->disconnect.reason);
        s_connected.store(false);
        s_advertising.store(false);
        s_connection_handle.store(BLE_HS_CONN_HANDLE_NONE);
        s_rssi_valid.store(false);
        clear_peer();
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising.store(false);
        start_advertising();
        return 0;

    default:
        return 0;
    }
}

void host_on_reset(int reason)
{
    ESP_LOGW(kTag, "BLE host reset, reason=%d", reason);
    s_connected.store(false);
    s_advertising.store(false);
    s_connection_handle.store(BLE_HS_CONN_HANDLE_NONE);
    s_rssi_valid.store(false);
    clear_peer();
}

void host_on_sync()
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE(kTag, "BLE address initialization failed, rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0)
    {
        ESP_LOGE(kTag, "BLE address type inference failed, rc=%d", rc);
        return;
    }
    ESP_LOGI(kTag, "BLE host synchronized, own_addr_type=%u",
             static_cast<unsigned>(s_own_addr_type));
    start_advertising();
}

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void start_advertising()
{
    if (!s_host_initialized.load() || s_connected.load() ||
        s_advertising.exchange(true))
        return;

    /* Legacy advertising data is limited to 31 bytes. Keep the advertised
     * name in the primary packet so passive scanners can show it, and put the
     * 128-bit service UUID in the scan response packet. */
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = reinterpret_cast<std::uint8_t *>(const_cast<char *>(kAdvertisedName));
    fields.name_len = sizeof(kAdvertisedName) - 1U;
    fields.name_is_complete = 1;

    ble_hs_adv_fields scan_fields{};
    scan_fields.uuids128 = const_cast<ble_uuid128_t *>(&s_service_uuid);
    scan_fields.num_uuids128 = 1;
    scan_fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
        ESP_LOGE(kTag, "BLE primary advertising fields failed, rc=%d", rc);
    if (rc == 0)
    {
        rc = ble_gap_adv_rsp_set_fields(&scan_fields);
        if (rc != 0)
            ESP_LOGE(kTag, "BLE scan response fields failed, rc=%d", rc);
    }
    if (rc == 0)
    {
        ble_gap_adv_params params{};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER,
                               &params, gap_event, nullptr);
        if (rc != 0)
            ESP_LOGE(kTag, "BLE advertising start failed, rc=%d", rc);
    }
    if (rc != 0)
        s_advertising.store(false);
    else
        ESP_LOGI(kTag, "BLE advertising started as %s", kAdvertisedName);
}

esp_err_t start_host()
{
    if (s_host_initialized.load())
        return ESP_OK;

    const esp_err_t init_result = nimble_port_init();
    if (init_result != ESP_OK)
        return init_result;

    ble_hs_cfg.reset_cb = host_on_reset;
    ble_hs_cfg.sync_cb = host_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0)
        rc = ble_gatts_add_svcs(s_services);
    if (rc == 0)
        rc = ble_svc_gap_device_name_set(kAdvertisedName);
    if (rc != 0)
    {
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }

    s_connected.store(false);
    s_advertising.store(false);
    s_connection_handle.store(BLE_HS_CONN_HANDLE_NONE);
    s_rssi_valid.store(false);
    clear_peer();
    s_host_initialized.store(true);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(kTag, "BLE host initialized; advertising will start after sync");
    return ESP_OK;
}

esp_err_t stop_host()
{
    if (!s_host_initialized.exchange(false))
        return ESP_OK;

    s_connected.store(false);
    s_advertising.store(false);
    s_connection_handle.store(BLE_HS_CONN_HANDLE_NONE);
    s_rssi_valid.store(false);
    clear_peer();
    const int stop_result = nimble_port_stop();
    const esp_err_t deinit_result = nimble_port_deinit();
    if (stop_result != 0)
        return ESP_FAIL;
    return deinit_result;
}
}

extern "C" bool bluetooth_control_is_enabled(void)
{
    const auto *connectivity = radio();
    return connectivity != nullptr &&
           connectivity->status().bluetooth_controller_enabled;
}

extern "C" bool bluetooth_control_is_connected(void)
{
    return bluetooth_control_is_enabled() && s_connected.load();
}

extern "C" esp_err_t bluetooth_control_get_status(char *buffer, size_t buffer_size)
{
    if (buffer == nullptr || buffer_size == 0U)
        return ESP_ERR_INVALID_ARG;

    if (!bluetooth_control_is_enabled())
        std::snprintf(buffer, buffer_size, "BLE OFF");
    else if (s_connected.load())
        std::snprintf(buffer, buffer_size, "BLE ON // CONNECTED");
    else if (s_advertising.load())
        std::snprintf(buffer, buffer_size, "BLE ON // ADVERTISING");
    else
        std::snprintf(buffer, buffer_size, "BLE ON");
    return ESP_OK;
}

extern "C" esp_err_t bluetooth_control_set_enabled(bool enabled)
{
    auto *connectivity = radio();
    if (connectivity == nullptr || !connectivity->isInitialized())
        return ESP_ERR_INVALID_STATE;

    const bool current = bluetooth_control_is_enabled();
    if (enabled == current)
        return ESP_OK;

    if (enabled)
    {
        esp_err_t result = connectivity->startBluetoothController();
        if (result != ESP_OK)
            return result;
        result = start_host();
        if (result != ESP_OK)
        {
            (void)connectivity->stopBluetoothController();
            return result;
        }
        return ESP_OK;
    }

    const esp_err_t host_result = stop_host();
    if (host_result != ESP_OK)
        return host_result;
    return connectivity->stopBluetoothController();
}

extern "C" esp_err_t bluetooth_control_get_info(bluetooth_control_info_t *info)
{
    if (info == nullptr)
        return ESP_ERR_INVALID_ARG;

    std::memset(info, 0, sizeof(*info));
    info->enabled = bluetooth_control_is_enabled();
    std::snprintf(info->advertised_name, sizeof(info->advertised_name), "%s",
                  kAdvertisedName);
    std::snprintf(info->local_address, sizeof(info->local_address), "%s",
                  kUnavailable);
    std::snprintf(info->host_identifier, sizeof(info->host_identifier), "%s",
                  kUnavailable);
    info->connected = s_connected.load();

    if (info->connected && s_peer_valid.load())
    {
        const std::uint64_t address = s_peer_address.load();
        const auto address_byte = [address](std::size_t index) {
            return static_cast<unsigned>((address >> (index * 8U)) & 0xffU);
        };
        std::snprintf(info->host_identifier, sizeof(info->host_identifier),
                      "CENTRAL %02X:%02X:%02X:%02X:%02X:%02X",
                      address_byte(5U), address_byte(4U), address_byte(3U),
                      address_byte(2U), address_byte(1U), address_byte(0U));
    }

    const auto *connectivity = radio();
    if (connectivity == nullptr || !connectivity->isInitialized())
        return ESP_OK;

    const size_t mac_length = esp_hosted_iface_mac_addr_len_get(ESP_MAC_BT);
    std::uint8_t mac[8]{};
    if (mac_length == 6U &&
        esp_hosted_iface_mac_addr_get(mac, mac_length, ESP_MAC_BT) == ESP_OK)
    {
        std::snprintf(info->local_address, sizeof(info->local_address),
                      "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (info->connected)
    {
        std::int8_t rssi{};
        const std::uint16_t handle = s_connection_handle.load();
        if (handle != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_rssi(handle, &rssi) == 0)
        {
            info->rssi_valid = true;
            info->rssi_dbm = rssi;
            s_rssi_dbm.store(rssi);
            s_rssi_valid.store(true);
        }
    }
    if (!info->rssi_valid && s_rssi_valid.load())
    {
        info->rssi_valid = true;
        info->rssi_dbm = s_rssi_dbm.load();
    }
    return ESP_OK;
}
