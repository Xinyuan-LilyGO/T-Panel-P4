# LR2021 LoRa transmit example

This example initializes the T-Panel-P4 Standard BSP and its LR2021 radio,
then transmits a numbered text packet every five seconds. GPIO32 enables the
board's external transmit path only while `radio.transmit()` is running and is
driven low immediately afterward, including when transmission fails.

The checked-in configuration uses 868 MHz, 125 kHz bandwidth, spreading
factor 7, coding rate 4/5, and 22 dBm output power. Change the frequency and
power in `main.cpp` before transmitting if these settings are not permitted in
your region. Connect a suitable antenna before running the example.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 lr2021 build
.\build.ps1 lr2021 flash-monitor COM198
```

The isolated build output is stored in `build/lr2021`.
