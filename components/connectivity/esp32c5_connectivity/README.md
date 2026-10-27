# ESP32-C5 connectivity

This component connects the ESP32-P4 host to an ESP32-C5 co-processor over a
four-bit SDIO bus. It intentionally builds on Espressif's supported protocol:

- `espressif/esp_hosted` owns SDIO transport, RPC and Hosted HCI framing.
- `espressif/esp_wifi_remote` maps the standard `esp_wifi_*` API to the C5.
- `Esp32c5Connectivity` owns application-facing lifecycle, validation, status,
  Wi-Fi station setup, Bluetooth Controller setup and recovery orchestration.
- The BSP owns physical pin declarations and board power/reset wiring.

The C5 must be flashed with the ESP-Hosted slave firmware from the same major
component generation. ESP-Hosted 2.12.13 is selected by this component's
manifest. At startup, the driver reads the remote target and firmware version,
rejects a target other than `esp32c5`, and can enforce a minimum or exact
ESP-Hosted version.

ESP-Hosted 2.12.9 and newer use an IDF 6.1-era parser macro in one unrelated
SPI-HD Kconfig condition. This component defines that parser macro for IDF 5.5;
no managed-component source is patched.

## Build configuration

The host project must select ESP32-C5, SDIO slot 1 and the board's four-bit bus
in `sdkconfig.defaults`. The included `sdio_esp32c5` example configures:

| Signal | ESP32-P4 GPIO |
| --- | ---: |
| SDIO CLK | 18 |
| SDIO CMD | 19 |
| SDIO D0..D3 | 14, 15, 16, 17 |
| ESP32-C5 EN/reset | 54 |

The example also enables NimBLE Host with ESP-Hosted VHCI. ESP32-C5 supports
BLE, not Classic Bluetooth.

## Lifecycle

```cpp
#include <esp32c5_connectivity/esp32c5_connectivity.hpp>

connectivity::Esp32c5Connectivity radio;
connectivity::Esp32c5ConnectivityConfig config{};
ESP_ERROR_CHECK(radio.initialize(config));

connectivity::Esp32c5WifiStationConfig station{};
ESP_ERROR_CHECK(connectivity::Esp32c5Connectivity::makeWifiStationConfig(
    "ssid", "password", station));
ESP_ERROR_CHECK(radio.startWifiStation(station));
ESP_ERROR_CHECK(radio.waitForWifiConnection(15000));

ESP_ERROR_CHECK(radio.stopWifi());
ESP_ERROR_CHECK(radio.deinitialize());
```

`initialize()` initializes NVS, `esp_netif`, and the default event loop when
requested, starts ESP-Hosted, connects to the C5, validates SDIO wiring and
reads the remote firmware information. It never erases NVS automatically.

The ESP-Hosted component initializes its transport from Kconfig before
`app_main`, so SDIO pin assignment is compile-time configuration. The runtime
configuration supplied here is an expected wiring contract and is checked
against the active ESP-Hosted configuration.

## Bluetooth

`startBluetoothController()` initializes and enables the Controller on the C5.
The application then starts NimBLE or Bluedroid on the P4. The application must
stop its Bluetooth Host stack before calling `stopBluetoothController()`.

Automatic transport recovery cannot recreate application-owned GATT services
or Host-stack state. When Bluetooth is active, provide a recovery hook:

```cpp
esp_err_t bluetoothRecovery(
    connectivity::Esp32c5RecoveryPhase phase,
    void *context)
{
    if (phase == connectivity::Esp32c5RecoveryPhase::before_transport_restart)
        return stopApplicationNimbleHost(context);
    return startApplicationNimbleHost(context);
}
```

Without a hook, recovery is rejected while Bluetooth is active instead of
silently leaving the Host and remote Controller out of sync.

## Recovery behavior

ESP-Hosted transport failure/down events wake a dedicated recovery task. It
serializes recovery with public operations, tears down Wi-Fi and the remote
Bluetooth Controller, reconnects and revalidates the C5, then restores owned
services. Wi-Fi station disconnects use the lighter `esp_wifi_connect()` retry
path first. `status()` exposes the last error, transport state, Wi-Fi state,
heartbeat, recovery count and reconnect count.

The example is located at `main/examples/sdio_esp32c5`. Set `WIFI_SSID` and
`WIFI_PASSWORD` at the top of `main.cpp`; it validates the C5 identity,
connects the remote Wi-Fi station, synchronizes time from `pool.ntp.org`, and
prints the current China Standard Time.
