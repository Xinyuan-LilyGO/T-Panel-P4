# T-Panel-P4

## Project overview

T-Panel-P4 is an ESP-IDF project for ESP32-P4 touch-panel development
boards, requiring ESP-IDF 5.5.3 or later. It provides a shared board support package (BSP), reusable peripheral drivers,
and example applications for display, touch, camera, audio, SD card, USB MSC,
LoRa, LVGL, and ESP32-C5 connectivity. The project supports the `standard`,
`round`, and `rect` hardware selections. All three use the [`t_panel_p4` BSP](components/bsp/t_panel_p4/README.md),
with a separate pin map and SDK defaults for each hardware version.

### Hardware and display specifications

The following values are taken from the BSP headers, panel initialization
tables, and example `sdkconfig.defaults` files in this repository.

| Item               | T-Panel-P4 Standard | T-Panel-P4 Round  | T-Panel-P4 Rect    |
| ------------------ | ------------------- | ----------------- | ------------------ |
| Hardware selection | `standard`          | `round`           | `rect`             |
| Main controller    | ESP32-P4(V3.2)      | ESP32-P4(V3.2)    | ESP32-P4(V3.2)     |
| SPI NOR Flash      | 16 MB               | 16 MB             | 16 MB              |
| PSRAM              | 32MB                | 32MB              | 32MB               |
| Display panel      | JD9365TX            | JD9365DA          | ILI9882            |
| Display size       | 5.08 inches         | 4.0 inches        | 5.5 inches         |
| Resolution         | 720 × 720           | 720 × 720         | 720 × 1440         |
| Pixel format       | RGB565 / RGB888     | RGB565 / RGB888   | RGB565 / RGB888    |
| Interface          | MIPI-DSI, 2 lanes   | MIPI-DSI, 2 lanes | MIPI-DSI, 2 lanes  |
| Touch controller   | Jadard              | GT911             | ILI9882            |
| Audio codec        | ES8389              | ES8389            | ES8389, ES7210     |
| Camera sensor      | OV2710              | OV2710            | OV2710             |
| Slave              | ESP32C5             | ESP32C5           | ---                |
| LORA Module        | LR2021              | LR2021            | ---                |
| Sensor             | ---                 | ---               | QMI8656C & QMC6309 |
| Motor              | ---                 | ---               | DRV2605            |

ESP-IDF examples are stored under `main/examples`. The root project selects
one hardware profile for the shared BSP and one example directory at configure time.
The selection logic is kept in `cmake/t_panel_p4_config.cmake`.
Build from the repository root; the directories under `main/examples` contain
example fragments, not standalone ESP-IDF projects.

## Hardware application scenarios (Standard / Round)

The existing hardware and examples provide starting points for the following
applications. Product-specific features require further development.

| Application                                    | Hardware capabilities and example entry points                                                                                                                                                                                              |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Smart-home control panels and device HMIs      | Standard's 5.08-inch square display suits multifunction control interfaces; Round's 4.0-inch circular display suits gauges, status displays, and dial-style touch interfaces. Start with `lcd_touch`, `lvgl_lcd_touch`, or `lvgl_fps_test`. |
| Connected displays and wireless device control | The ESP32-C5 provides Wi-Fi and BLE over SDIO. `sdio_esp32c5` demonstrates networking, while `factory_standard` / `factory_round` provide integrated user interfaces. The C5 requires matching ESP-Hosted slave firmware.                   |
| LoRa data terminals and status displays        | Use the LR2021 for long-range wireless data exchange, with the touchscreen displaying readings or configuring parameters; see `lr2021`.                                                                                                     |
| Camera preview and audio terminals             | Combine the OV2710 camera, ES8389 audio codec, and display for preview, recording, and playback; see `camera`, `audio/loopback`, `audio/play_wav`, and `audio/play_sd_mp3`.                                                                 |
| Local media storage and file transfer          | Store files and media on an SD card, or expose the card to a computer as USB mass storage; see `sd` and `usb_msc`. The USB host has exclusive access to the SD card while USB MSC is active.                                                |

## Pin mapping (Standard / Round)

The tables below follow the BSP's [Standard pin definitions](components/bsp/t_panel_p4/include/bsp/pins/standard.hpp)
and [Round pin definitions](components/bsp/t_panel_p4/include/bsp/pins/round.hpp).
Both boards share these mappings. `GPIO<n>` refers to an ESP32-P4 pin;
`XL9555 P0_n` refers to an I/O expander port and must not be used as a host GPIO number.

### Host GPIOs

| Peripheral / signal             | Standard                          | Round                             | Rect |
| ------------------------------- | --------------------------------- | --------------------------------- | ---- |
| Shared I2C SDA / SCL            | GPIO3 / GPIO2                     | GPIO3 / GPIO2                     | --   |
| LR2021 SPI SCK / MISO / MOSI    | GPIO28 / GPIO27 / GPIO26          | GPIO28 / GPIO27 / GPIO26          | --   |
| LR2021 CS                       | GPIO29                            | GPIO29                            | --   |
| LR2021 BUSY                     | GPIO30                            | GPIO30                            | --   |
| LR2021 RESET                    | GPIO31                            | GPIO31                            | --   |
| LR2021 IRQ (module DIO10)       | GPIO5                             | GPIO5                             | --   |
| Audio I2S DOUT (P4 → codec)     | GPIO9                             | GPIO9                             | --   |
| Audio I2S DIN (codec → P4)      | GPIO11                            | GPIO11                            | --   |
| Audio I2S WS / BCLK / MCLK      | GPIO10 / GPIO12 / GPIO13          | GPIO10 / GPIO12 / GPIO13          | --   |
| LCD TE                          | GPIO33                            | GPIO33                            | --   |
| LCD backlight PWM               | GPIO34                            | GPIO34                            | --   |
| Touch INT                       | GPIO6                             | GPIO6                             | --   |
| AXP517 INT                      | GPIO4                             | GPIO4                             | --   |
| BOOT button                     | GPIO35                            | GPIO35                            | --   |
| User button                     | GPIO36                            | GPIO36                            | --   |
| ESP32-C5 EN / RESET             | GPIO54                            | GPIO54                            | --   |
| SD card CLK / CMD               | GPIO43 / GPIO44                   | GPIO43 / GPIO44                   | --   |
| SD card D0 / D1 / D2 / D3       | GPIO39 / GPIO40 / GPIO41 / GPIO42 | GPIO39 / GPIO40 / GPIO41 / GPIO42 | --   |
| ESP32-C5 SDIO CLK / CMD         | GPIO18 / GPIO19                   | GPIO18 / GPIO19                   | --   |
| ESP32-C5 SDIO D0 / D1 / D2 / D3 | GPIO14 / GPIO15 / GPIO16 / GPIO17 | GPIO14 / GPIO15 / GPIO16 / GPIO17 | --   |

### XL9555 ports (I2C address `0x20`)

| Control signal                             | Standard    | Round       |
| ------------------------------------------ | ----------- | ----------- |
| Speaker amplifier control                  | XL9555 P0_0 | XL9555 P0_0 |
| SD card power enable                       | XL9555 P0_1 | XL9555 P0_1 |
| Shared ESP32-C5 / audio / LCD power enable | XL9555 P0_2 | XL9555 P0_2 |
| LCD reset                                  | XL9555 P0_3 | XL9555 P0_3 |
| Touch reset                                | XL9555 P0_4 | XL9555 P0_4 |
| LoRa power enable                          | XL9555 P0_5 | XL9555 P0_5 |

LCD image data uses MIPI-DSI, and camera image data uses MIPI-CSI. These
high-speed signals use dedicated interfaces and are not listed as ordinary GPIOs.
For connectors and complete wiring, see the matching board revision's schematic in `hardware/`.

## Project structure

| Directory / file                                | Main responsibility                                                                                                                                                                                |
| ----------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `CMakeLists.txt`, `build.ps1`                   | Root build entry points; select hardware and example, isolate build directories, and generate merged firmware.                                                                                     |
| `cmake/`                                        | Hardware / example selection, hardware compatibility checks, and the main-component template for the selected example.                                                                             |
| `main/CMakeLists.txt`, `main/examples/`         | Register the selected example's sources and dependencies; contain individual peripheral examples and the integrated `factory_standard` and `factory_round` applications.                           |
| `components/bsp/t_panel_p4/`                    | Shared BSP; `include/bsp/` provides board APIs and pin definitions, `src/` handles power sequencing and bus / peripheral initialization, and `profiles/` holds board defaults.                     |
| `components/peripheral_drivers_lib/`            | Chip drivers; `include/pdl/` defines interfaces, `src/drivers/` implements display, touch, audio, power, and other drivers, and `port/esp_idf/` adapts ESP-IDF buses, delays, and GPIO interrupts. |
| `components/connectivity/esp32c5_connectivity/` | Manage P4-to-C5 SDIO communication, Wi-Fi, the Bluetooth Controller, and connection recovery using ESP-Hosted and esp_wifi_remote.                                                                 |
| `components/lvgl_page_manager/`                 | LVGL page registration, navigation, and page lifecycle management.                                                                                                                                 |
| `managed_components/`, `dependencies.lock`      | Third-party dependencies downloaded by ESP-IDF Component Manager and their locked versions.                                                                                                        |
| `hardware/`, `doc/`                             | Board schematics, chip / display datasheets, and reference material.                                                                                                                               |
| `firmware/`                                     | Prebuilt firmware, including the ESP32-C5 ESP-Hosted slave image.                                                                                                                                  |
| `.vscode/`                                      | ESP-IDF extension settings and build, flash, and monitor tasks.                                                                                                                                    |
| `partitions.csv`, `build/`                      | Flash partition layout; generated build outputs, configuration, and caches organized by hardware and example.                                                                                      |

## Supported build environment

- **ESP-IDF: 5.5.3 or later**; versions below 5.5.3 are not supported. Use 5.5.3 as the recommended build baseline.
- **Target chip: `esp32p4`**.
- **Command line: a terminal with the ESP-IDF environment exported**. The supplied `build.ps1` and VS Code tasks use PowerShell; load `export.ps1` from the intended ESP-IDF installation first.
- **IDE: VS Code with the Espressif ESP-IDF extension**, also configured to use ESP-IDF 5.5.3 or later.

Run `idf.py --version` to confirm the ESP-IDF version active in the terminal
before using one of the build methods below.

## Quick build

Run these commands from an exported ESP-IDF PowerShell in the repository root:

```powershell
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action build
.\build.ps1 -HardwareVersion standard -Example axp517 -Action build
.\build.ps1 -HardwareVersion standard -Example xl9555 -Action build
.\build.ps1 -HardwareVersion round -Example factory_round -Action build
.\build.ps1 -HardwareVersion rect -Example lcd_touch -Action build
.\build.ps1 -HardwareVersion rect -Example rect_lowpower_test -Action build
```

Do not run `idf.py build` from `main/examples/<name>`. Use `build.ps1`, or run
the equivalent root-project command shown below.

The outputs are kept in separate subdirectories under `build`:

```text
build/standard-factory_standard/
build/standard-axp517/
build/standard-xl9555/
build/round-factory_round/
build/rect-lcd_touch/
build/rect-rect_lowpower_test/
```

The script also supports common IDF actions:

```powershell
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action flash -Port COM7
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action monitor -Port COM7
.\build.ps1 -HardwareVersion standard -Example axp517 -Action flash-monitor -Port COM7
.\build.ps1 -HardwareVersion standard -Example xl9555 -Action flash-monitor -Port COM7
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action menuconfig
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action fullclean
```

The VS Code command palette also provides these tasks:

- `ESP-IDF: Build example`
- `ESP-IDF: Flash example`
- `ESP-IDF: Flash and monitor example`
- `ESP-IDF: Menuconfig example`

Each task asks which hardware version and example to use. Flash tasks also ask
for the serial port.
The tasks load `export.ps1` from the ESP-IDF path configured by the ESP-IDF
extension before invoking the build script. Run `ESP-IDF: Configure ESP-IDF
extension` once before using the tasks. If no extension path is configured,
the tasks fall back to an already-exported `IDF_PATH` environment.

## Three build methods

The project supports three build methods. All three use the same root
`CMakeLists.txt`, hardware selection, example selection, and isolated build
directory naming.

| Method                       | Entry point              | Select hardware/example                        | Build directory        | Best use                      |
| ---------------------------- | ------------------------ | ---------------------------------------------- | ---------------------- | ----------------------------- |
| ESP-IDF terminal             | `build.ps1`              | Command-line parameters                        | Selected automatically | Terminal, scripts, and CI     |
| VS Code task                 | `ESP-IDF: Build example` | Pick hardware and example when the task starts | Selected automatically | Frequently switching variants |
| VS Code ESP-IDF Build button | ESP-IDF `Build Project`  | Edit `.vscode/settings.json` first             | Set by `idf.buildPath` | Rebuilding one fixed variant  |

Each hardware/example pair has its own CMake and Ninja cache. Rebuilding the
same pair reuses that cache; changing either selection uses a different build
directory and does not invalidate the other variants.

### 1. ESP-IDF terminal

Run the common entry point from an exported ESP-IDF PowerShell:

Command format:

```text
./build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action build
./build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action flash -Port <COMX>
./build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action flash-monitor -Port <COMX>
./build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action menuconfig
./build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action fullclean
```

```powershell
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action build
.\build.ps1 -HardwareVersion round -Example factory_round -Action build
.\build.ps1 -HardwareVersion rect -Example lcd_touch -Action build
```

The script creates the build directory from both selections, for example
`build/standard-factory_standard` or `build/rect-lcd_touch`.

Normal builds automatically generate `T-Panel-P4-<hardware>-<example>-merged.bin`
in the selected build directory, for example
`T-Panel-P4-standard-factory_standard-merged.bin`. Slashes in example paths are
replaced with hyphens. It includes the bootloader, partition table, and application, and can
be flashed at address `0x0`. `T-Panel-P4-<example>.bin` remains the application
image; use the address recorded in the same directory's `flash_args` for it.

### 2. VS Code task

Open the Command Palette and run `Tasks: Run Task`, then choose
`ESP-IDF: Build example`. The task asks for the hardware version and example
directory, then invokes the same `build.ps1` entry point used by the terminal.
This is the recommended method when switching between several variants.

Before using the task for the first time, run
`ESP-IDF: Configure ESP-IDF extension` so the task can locate the configured
ESP-IDF installation. If the extension path is not configured, the task uses
the already-exported `IDF_PATH` environment.

### 3. VS Code ESP-IDF Build button

The ESP-IDF extension's `Build Project` button can build the variant configured
in `.vscode/settings.json`. It does not display the hardware/example picker;
edit the settings before building another variant.

For example, to build the `rect` hardware with the `lcd_touch` example, set
the build paths and CMake arguments together:

```json
{
  "idf.buildPath": "${workspaceFolder}/build/rect-lcd_touch",
  "idf.buildPathWin": "${workspaceFolder}\\build\\rect-lcd_touch",
  "idf.cmakeCompilerArgs": [
    "-G",
    "Ninja",
    "-DPYTHON_DEPS_CHECKED=1",
    "-DESP_PLATFORM=1",
    "-DT_PANEL_P4_HARDWARE_VERSION=rect",
    "-DT_PANEL_P4_EXAMPLE=lcd_touch"
  ],
  "idf.customExtraVars": {
    "IDF_TARGET": "esp32p4",
    "IDF_COMPONENT_MANAGER": "1"
  }
}
```

On Windows, keep both `idf.buildPath` and `idf.buildPathWin` synchronized.
The build directory name must match the selected hardware and example. After
changing these settings, run `ESP-IDF: Configure ESP-IDF extension` once and
then click `Build Project`.

For example, for `standard + axp517`, use:

```json
"idf.buildPath": "${workspaceFolder}/build/standard-axp517",
"idf.buildPathWin": "${workspaceFolder}\\build\\standard-axp517",
"-DT_PANEL_P4_HARDWARE_VERSION=standard",
"-DT_PANEL_P4_EXAMPLE=axp517"
```

Do not change only one of the build path, hardware argument, or example
argument. Keep all of them aligned. Otherwise CMake may report that the build
directory was configured for a different variant.

The equivalent direct `idf.py` commands are:

```powershell
idf.py -B build/standard-factory_standard `
    -D T_PANEL_P4_HARDWARE_VERSION=standard `
    -D T_PANEL_P4_EXAMPLE=factory_standard build
idf.py -B build/rect-lcd_touch `
    -D T_PANEL_P4_HARDWARE_VERSION=rect `
    -D T_PANEL_P4_EXAMPLE=lcd_touch build
idf.py -B build/round-factory_round `
    -D T_PANEL_P4_HARDWARE_VERSION=round `
    -D T_PANEL_P4_EXAMPLE=factory_round build
```

## Add an example

Create `main/examples/<name>/CMakeLists.txt` and append the example's sources
and private dependencies:

```cmake
list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    peripheral_drivers_lib
    t_panel_p4
)
```

Then build it with `./build.ps1 -HardwareVersion standard -Example <name>`.
Its output will be written to `build/standard-<name>`.

