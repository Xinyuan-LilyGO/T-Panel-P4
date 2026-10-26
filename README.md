# T-Panel-P4

ESP-IDF examples are stored under `main/examples`. The root project selects
one example at configure time and gives each example its own build directory.
Build from the repository root; the directories under `main/examples` contain
example fragments, not standalone ESP-IDF projects.

## Quick build

Run these commands from an exported ESP-IDF PowerShell in the repository root:

```powershell
.\build.ps1 -Example factory_standard -Action build
.\build.ps1 -Example unit_test -Action build
.\build.ps1 -Example axp517 -Action build
.\build.ps1 -Example xl9555 -Action build
.\build.ps1 -Example usb_msc -Action build
```

Do not run `idf.py build` from `main/examples/<name>`. Use `build.ps1`, or run
the equivalent root-project command shown below.

The outputs are kept in separate subdirectories under `build`:

```text
build/factory_standard/
build/unit_test/
build/axp517/
build/xl9555/
build/usb_msc/
```

The script also supports common IDF actions:

```powershell
.\build.ps1 -Example factory_standard -Action flash -Port COM7
.\build.ps1 -Example factory_standard -Action monitor -Port COM7
.\build.ps1 -Example unit_test -Action flash-monitor -Port COM7
.\build.ps1 -Example axp517 -Action flash-monitor -Port COM7
.\build.ps1 -Example xl9555 -Action flash-monitor -Port COM7
.\build.ps1 -Example factory_standard -Action menuconfig
.\build.ps1 -Example factory_standard -Action fullclean
```

The VS Code command palette also provides these tasks:

- `ESP-IDF: Build example`
- `ESP-IDF: Flash example`
- `ESP-IDF: Flash and monitor example`
- `ESP-IDF: Menuconfig example`

Each task asks which example to use. Flash tasks also ask for the serial port.
The tasks load `export.ps1` from the ESP-IDF path configured by the ESP-IDF
extension before invoking the build script. Run `ESP-IDF: Configure ESP-IDF
extension` once before using the tasks. If no extension path is configured,
the tasks fall back to an already-exported `IDF_PATH` environment.

The equivalent direct `idf.py` commands are:

```powershell
idf.py -B build/factory_standard -D T_PANEL_P4_EXAMPLE=factory_standard build
idf.py -B build/unit_test -D T_PANEL_P4_EXAMPLE=unit_test build
idf.py -B build/axp517 -D T_PANEL_P4_EXAMPLE=axp517 build
idf.py -B build/xl9555 -D T_PANEL_P4_EXAMPLE=xl9555 build
idf.py -B build/camera -D T_PANEL_P4_EXAMPLE=camera build
idf.py -B build/audio/play_wav -D T_PANEL_P4_EXAMPLE=audio/play_wav build
idf.py -B build/usb_msc -D T_PANEL_P4_EXAMPLE=usb_msc build
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
)
```

Then build it with `./build.ps1 <name>`. Its output will be written to
`build/<name>`.

