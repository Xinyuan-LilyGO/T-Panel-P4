# T-Panel-P4

## Project overview

T-Panel-P4 is an ESP-IDF 5.5 project for ESP32-P4 touch-panel development
boards. It provides board support packages (BSPs), reusable peripheral drivers,
and example applications for display, touch, camera, audio, SD card, USB MSC,
LoRa, LVGL, and ESP32-C5 connectivity. The project supports the `standard`,
`round`, and `rect` hardware selections. The `round` hardware uses the separate
`t_panel_p4_round` BSP component while keeping the standard BSP's public headers
and namespace, so existing standard examples can be reused directly.

### Hardware and display specifications

The following values are taken from the BSP headers, panel initialization
tables, and example `sdkconfig.defaults` files in this repository.

| Item               | T-Panel-P4 Standard   | T-Panel-P4 Round   | T-Panel-P4 Rect    |
| ------------------ | --------------------- | ------------------ | ------------------ |
| Hardware selection | `standard`            | `round`            | `rect`             |
| BSP component      | `t_panel_p4_standard` | `t_panel_p4_round` | `t_panel_p4_rect`  |
| Main controller    | ESP32-P4(V3.2)        | ESP32-P4(V3.2)     | ESP32-P4(V3.2)     |
| SPI NOR Flash      | 16 MB                 | 16 MB              | 16 MB              |
| PSRAM              | 32MB                  | 32MB               | 32MB               |
| Display panel      | JD9365TX              | JD9365DA           | ILI9882            |
| Display size       | 5.08 inches           | 4.0 inches         | 5.5 inches         |
| Resolution         | 720 × 720             | 720 × 720          | 720 × 1440         |
| Pixel format       | RGB565 / RGB888       | RGB565 / RGB888    | RGB565 / RGB888    |
| Interface          | MIPI-DSI, 2 lanes     | MIPI-DSI, 2 lanes  | MIPI-DSI, 2 lanes  |
| Touch controller   | Jadard                | GT911              | ILI9882            |
| Audio codec        | ES8389                | ES8389             | ES8389, ES7210     |
| Camera sensor      | OV2710                | OV2710             | OV2710             |
| Slave              | ESP32C5               | ESP32C5            | ---                |
| LORA Module        | LR2021                | LR2021             | ---                |
| Sensor             | ---                   | ---                | QMI8656C & QMC6309 |
| Motor              | ---                   | ---                | DRV2605            |

ESP-IDF examples are stored under `main/examples`. The root project selects
one hardware version, its BSP, and one example directory at configure time.
The selection logic is kept in `cmake/t_panel_p4_config.cmake`.
Build from the repository root; the directories under `main/examples` contain
example fragments, not standalone ESP-IDF projects.

## Quick build

Run these commands from an exported ESP-IDF PowerShell in the repository root:

```powershell
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action build
.\build.ps1 -HardwareVersion standard -Example axp517 -Action build
.\build.ps1 -HardwareVersion standard -Example xl9555 -Action build
.\build.ps1 -HardwareVersion round -Example factory_standard -Action build
.\build.ps1 -HardwareVersion rect -Example lcd/ili9882 -Action build
.\build.ps1 -HardwareVersion rect -Example rect_lowpower_test -Action build
```

Do not run `idf.py build` from `main/examples/<name>`. Use `build.ps1`, or run
the equivalent root-project command shown below.

The outputs are kept in separate subdirectories under `build`:

```text
build/standard-factory_standard/
build/standard-axp517/
build/standard-xl9555/
build/round-factory_standard/
build/rect-lcd-ili9882/
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
.\build.ps1 -HardwareVersion round -Example factory_standard -Action build
.\build.ps1 -HardwareVersion rect -Example lcd/ili9882 -Action build
```

The script creates the build directory from both selections, for example
`build/standard-factory_standard` or `build/rect-lcd-ili9882`.

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

For example, to build the `rect` hardware with the `lcd/ili9882` example, set
the build paths and CMake arguments together:

```json
{
  "idf.buildPath": "${workspaceFolder}/build/rect-lcd-ili9882",
  "idf.buildPathWin": "${workspaceFolder}\\build\\rect-lcd-ili9882",
  "idf.cmakeCompilerArgs": [
    "-G",
    "Ninja",
    "-DPYTHON_DEPS_CHECKED=1",
    "-DESP_PLATFORM=1",
    "-DT_PANEL_P4_HARDWARE_VERSION=rect",
    "-DT_PANEL_P4_EXAMPLE=lcd/ili9882"
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
idf.py -B build/rect-lcd-ili9882 `
    -D T_PANEL_P4_HARDWARE_VERSION=rect `
    -D T_PANEL_P4_EXAMPLE=lcd/ili9882 build
idf.py -B build/round-factory_standard `
    -D T_PANEL_P4_HARDWARE_VERSION=round `
    -D T_PANEL_P4_EXAMPLE=factory_standard build
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
    t_panel_p4_standard
)
```

Then build it with `./build.ps1 -HardwareVersion standard -Example <name>`.
Its output will be written to `build/standard-<name>`.

