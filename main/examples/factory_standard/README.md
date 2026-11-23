# Standard factory application

`app_main()` initializes the BSP devices through `Board::initialize(false)`. The
BSP owns power and bus setup, then initializes display, touch, audio, camera,
and LoRa in its defined order. The factory UI deliberately leaves SD-card
initialization optional so the board can boot without a card; file, recording,
and other `/sdcard` features report that the card is unavailable until an
application initializes it separately.

Every required BSP device must initialize successfully before the UI starts. A
failed camera or LoRa radio still fails startup. The BSP rolls back peripherals
created by the failed initialization call, and `ESP_ERROR_CHECK` reports the
failure. Previously initialized devices retain their configuration.

After board initialization, the application uses `reconfigureAudio()` to apply
its factory audio settings, including the 48 kHz full-duplex codec setup,
384x MCLK, output volume, microphone gain, DAC reference policy, and DMA sizes.
It then disables audio RX until recording is requested. ESP32-C5 connectivity
and Wi-Fi remain application-owned and start through `bsp::Esp32c5Slave`.

The camera is initialized at boot but does not stream until the Camera page is
entered. Leaving that page releases camera resources; entering it again uses
the BSP camera API to recreate them. The LoRa page similarly reuses the device
initialized at boot and can release it on exit. Peripheral startup logs report
the board device states and the application connectivity/audio results.

Build from an exported ESP-IDF shell:

```powershell
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action build
```
