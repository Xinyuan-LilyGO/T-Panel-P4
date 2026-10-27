# T-Panel-P4

## 项目简介
T-Panel-P4 是一个面向 ESP32-P4 触摸屏开发板的 ESP-IDF(V5.5)。
工程提供板级支持包（BSP）、可复用的外设驱动，以及显示、触摸、摄像头、音频、SD 卡、USB MSC、LoRa、LVGL 和 ESP32-C5 通信等示例应用。
目前通过 `standard`、`round` 和 `rect` 三个硬件选项支持不同的板卡形态。
`round` 使用独立的 `t_panel_p4_round` BSP 组件，同时保留标准 BSP 的公开头文件和命名空间，现有标准示例可以直接复用。

### 开发板
以下信息来自工程中的 BSP 头文件、屏幕初始化表，以及各示例的 `sdkconfig.defaults` 配置文件。

| 项目          | T-Panel-P4 Standard   | T-Panel-P4 Round   | T-Panel-P4 Rect    |
| ------------- | --------------------- | ------------------ | ------------------ |
| 硬件选项      | `standard`            | `round`            | `rect`             |
| BSP 组件      | `t_panel_p4_standard` | `t_panel_p4_round` | `t_panel_p4_rect`  |
| 主控芯片      | ESP32-P4(V3.2)        | ESP32-P4(V3.2)     | ESP32-P4(V3.2)     |
| SPI NOR Flash | 16 MB                 | 16 MB              | 16 MB              |
| 外部 PSRAM    | 32MB                  | 32MB               | 32MB               |
| 屏幕型号      | JD9365TX              | JD9365DA           | ILI9882            |
| 屏幕尺寸      | 5.08 英寸             | 4.0 英寸           | 5.5 英寸           |
| 屏幕分辨率    | 720 × 720             | 720 × 720          | 720 × 1440         |
| 像素格式      | RGB565/RGB888         | RGB565/RGB888      | RGB565/RGB888      |
| 接口          | MIPI-DSI，2lane       | MIPI-DSI，2lane    | MIPI-DSI，2lane    |
| 触摸控制器    | Jadard                | GT911              | ILI9882            |
| 音频编解码器  | ES8389                | ES8389             | ES8389，ES7210     |
| 摄像头传感器  | OV2710                | OV2710             | OV2710             |
| 从机          | ESP32C5               | ESP32C5            | ---                |
| lora模块      | LR2021                | LR2021             | ---                |
| 传感器        | ---                   | ---                | QMI8656C & QMC6309 |
| 电机          | ---                   | ---                | DRV2605            |

ESP-IDF 示例存放在 `main/examples` 目录下。根工程在配置阶段选择一个硬件版本、对应的 BSP，以及一个示例目录。
选择逻辑位于 `cmake/t_panel_p4_config.cmake`。
请在工程根目录执行编译；`main/examples` 下的目录是示例片段，并不是可以独立编译的 ESP-IDF 工程。

## 三种编译方式
工程支持三种构建方式。三种方式都使用相同的根目录 `CMakeLists.txt`、
硬件选择、示例选择，以及按变体隔离的构建目录命名规则。

| 构建方式                 | 入口                     | 如何选择硬件/示例              | 构建目录                | 适用场景             |
| ------------------------ | ------------------------ | ------------------------------ | ----------------------- | -------------------- |
| ESP-IDF 终端             | `build.ps1`              | 通过命令行参数选择             | 自动生成                | 终端、脚本和 CI      |
| VS Code 任务             | `ESP-IDF: Build example` | 启动任务时选择硬件和示例       | 自动生成                | 经常切换不同变体     |
| VS Code ESP-IDF 构建按钮 | ESP-IDF `Build Project`  | 先编辑 `.vscode/settings.json` | 由 `idf.buildPath` 指定 | 重复编译一个固定变体 |

每个硬件版本和示例组合都有独立的 CMake/Ninja 缓存。
再次编译相同组合时会复用缓存；切换任意一个选择时会使用新的构建目录，不会使其他变体失效。

### 1. ESP-IDF 终端
请在已经导出 ESP-IDF 环境的 PowerShell 终端中，从工程根目录执行以下命令：

命令格式： 
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action build
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action flash -Port <COMX>
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action flash-monitor -Port <COMX>
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action menuconfig
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action fullclean

```powershell
.\build.ps1 -HardwareVersion standrad -Example factory_standrad -Action build
.\build.ps1 -HardwareVersion standrad -Example factory_standrad -Action flash -Port COM159
.\build.ps1 -HardwareVersion standrad -Example factory_standrad -Action flash-monitor -Port COM159
.\build.ps1 -HardwareVersion standrad -Example factory_standrad -Action menuconfig
.\build.ps1 -HardwareVersion standrad -Example factory_standrad -Action fullclean
```

不要在 `main/examples/<name>` 目录中执行 `idf.py build`。请使用 `build.ps1`，或者使用下面所示的根工程 `idf.py` 等效命令。
编译输出会按照硬件版本和示例目录分别保存在 `build` 下的独立子目录中：

```text
build/standard-factory_standard/
```

### 2. VS Code 任务
VS Code 命令面板中也提供了以下任务：

- `ESP-IDF: Build example`
- `ESP-IDF: Flash example`
- `ESP-IDF: Flash and monitor example`
- `ESP-IDF: Menuconfig example`

每个任务都会要求选择硬件版本和示例目录。烧录任务还会要求输入串口号。
任务会先从 ESP-IDF 插件配置的路径加载 `export.ps1`，然后调用构建脚本。
第一次使用任务前，请先执行 `ESP-IDF: Configure ESP-IDF extension`。
如果插件没有配置 ESP-IDF 路径，任务会回退到当前已经导出的 `IDF_PATH` 环境变量。

1. `ctrl+shift+p`打开命令面板，执行 `Tasks: Run Task`，然后选择 `ESP-IDF: Build example`。
2. 任务会要求选择硬件版本和示例目录，然后调用与终端相同的 `build.ps1` 入口。
3. 如果需要在多个变体之间切换，推荐使用这种方式。

第一次使用任务前，请先执行 `ESP-IDF: Configure ESP-IDF extension`，
以便任务找到配置好的 ESP-IDF 安装路径。如果插件没有配置路径，任务会使用当前已经导出的 `IDF_PATH` 环境变量。

### 3. VS Code ESP-IDF 构建按钮
ESP-IDF 插件的 `Build Project` 按钮可以编译 `.vscode/settings.json` 中配置的变体。
它不会弹出硬件和示例选择框；如果需要编译其他变体，需要先修改配置文件。

例如，编译 `rect` 硬件的 `lcd/ili9882` 示例，需要同步设置构建路径和 CMake 参数：

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

在 Windows 下，`idf.buildPath` 和 `idf.buildPathWin` 必须保持一致。
构建目录名称也必须与硬件版本和示例目录匹配。修改后先执行一次
`ESP-IDF: Configure ESP-IDF extension`，然后点击 `Build Project`。

例如 `standard + axp517` 的配置如下：

```json
"idf.buildPath": "${workspaceFolder}/build/standard-axp517",
"idf.buildPathWin": "${workspaceFolder}\\build\\standard-axp517",
"-DT_PANEL_P4_HARDWARE_VERSION=standard",
"-DT_PANEL_P4_EXAMPLE=axp517"
```

不要只修改构建目录、硬件参数或示例参数中的一个，三者必须保持一致，否则 CMake 可能会报告构建目录属于其他变体。

对应的直接 `idf.py` 命令如下：
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

## 添加示例
创建 `main/examples/<name>/CMakeLists.txt`，并在其中添加示例源文件和私有依赖：

```cmake
list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    peripheral_drivers_lib
    t_panel_p4_standard
)
```

然后使用以下命令编译：

```powershell
.\build.ps1 -HardwareVersion standard -Example <name>
```

编译输出会写入 `build/standard-<name>`。
