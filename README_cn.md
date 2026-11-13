# T-Panel-P4

## 项目简介
T-Panel-P4 是一个面向 ESP32-P4 触摸屏开发板的 ESP-IDF 项目，要求 ESP-IDF 5.5.3 及以上版本。
工程提供板级支持包（BSP）、可复用的外设驱动，以及显示、触摸、摄像头、音频、SD 卡、USB MSC、LoRa、LVGL 和 ESP32-C5 通信等示例应用。
目前通过 `standard`、`round` 和 `rect` 三个硬件选项支持不同的板卡形态。
三款开发板共用 [`t_panel_p4` BSP](components/bsp/t_panel_p4/README.md)，每款板的引脚表和 SDK 默认配置单独保存。

### 开发板
以下信息来自工程中的 BSP 头文件、屏幕初始化表，以及各示例的 `sdkconfig.defaults` 配置文件。

| 项目          | T-Panel-P4 Standard | T-Panel-P4 Round | T-Panel-P4 Rect    |
| ------------- | ------------------- | ---------------- | ------------------ |
| 硬件选项      | `standard`          | `round`          | `rect`             |
| 主控芯片      | ESP32-P4(V3.2)      | ESP32-P4(V3.2)   | ESP32-P4(V3.2)     |
| SPI NOR Flash | 16 MB               | 16 MB            | 16 MB              |
| 外部 PSRAM    | 32MB                | 32MB             | 32MB               |
| 屏幕型号      | JD9365TX            | JD9365DA         | ILI9882            |
| 屏幕尺寸      | 5.08 英寸           | 4.0 英寸         | 5.5 英寸           |
| 屏幕分辨率    | 720 × 720           | 720 × 720        | 720 × 1440         |
| 像素格式      | RGB565/RGB888       | RGB565/RGB888    | RGB565/RGB888      |
| 接口          | MIPI-DSI，2lane     | MIPI-DSI，2lane  | MIPI-DSI，2lane    |
| 触摸控制器    | Jadard              | GT911            | ILI9882            |
| 音频编解码器  | ES8389              | ES8389           | ES8389，ES7210     |
| 摄像头传感器  | OV2710              | OV2710           | OV2710             |
| 从机          | ESP32C5             | ESP32C5          | ---                |
| lora模块      | LR2021              | LR2021           | ---                |
| 传感器        | ---                 | ---              | QMI8656C & QMC6309 |
| 电机          | ---                 | ---              | DRV2605            |

ESP-IDF 示例存放在 `main/examples` 目录下。根工程在配置阶段为共用 BSP 选择硬件配置，并选择一个示例目录。
选择逻辑位于 `cmake/t_panel_p4_config.cmake`。
请在工程根目录执行编译；`main/examples` 下的目录是示例片段，并不是可以独立编译的 ESP-IDF 工程。

## 硬件应用场景（Standard / Round）

以下场景可基于现有硬件和示例进行开发，具体产品功能需要在示例基础上扩展。

| 应用场景                       | 硬件能力与示例入口                                                                                                                                                     |
| ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 智能家居控制面板、设备人机界面 | Standard 的 5.08 英寸方形屏适合多功能控制界面；Round 的 4.0 英寸圆形屏适合仪表、状态显示和旋钮式触摸界面。可从 `lcd_touch`、`lvgl_lcd_touch`、`lvgl_fps_test` 入手。   |
| 联网显示与无线设备控制         | 通过 ESP32-C5 的 SDIO 连接使用 Wi-Fi 和 BLE；`sdio_esp32c5` 演示联网，`factory_standard` / `factory_round` 提供综合交互界面。ESP32-C5 需要配套的 ESP-Hosted 从机固件。 |
| LoRa 数据采集终端与状态看板    | LR2021 可用于远距离无线数据收发，再通过触摸屏显示数据或设置参数；参考 `lr2021`。                                                                                       |
| 摄像头预览与音频交互终端       | OV2710 摄像头、ES8389 音频编解码器和显示屏可用于预览、录音及播放；参考 `camera`、`audio/loopback`、`audio/play_wav`、`audio/play_sd_mp3`。                             |
| 本地媒体存储与文件传输         | SD 卡用于保存文件和媒体，USB MSC 可将 SD 卡作为电脑端的 USB 存储设备；参考 `sd`、`usb_msc`。USB MSC 工作时，SD 卡由 USB 主机独占。                                     |

## 引脚映射（Standard / Round）

下表依据 BSP 的 [Standard 引脚定义](components/bsp/t_panel_p4/include/bsp/pins/standard.hpp) 和
[Round 引脚定义](components/bsp/t_panel_p4/include/bsp/pins/round.hpp)，两款板的以下映射一致。
`GPIO<n>` 表示 ESP32-P4 主控引脚；`XL9555 P0_n` 表示 I/O 扩展器端口，不能作为主控 GPIO 编号使用。

### 主控 GPIO

| 外设 / 信号                     | Standard                          | Round                             | Rect |
| ------------------------------- | --------------------------------- | --------------------------------- | ---- |
| 共享 I2C SDA / SCL              | GPIO3 / GPIO2                     | GPIO3 / GPIO2                     | --   |
| LR2021 SPI SCK / MISO / MOSI    | GPIO28 / GPIO27 / GPIO26          | GPIO28 / GPIO27 / GPIO26          | --   |
| LR2021 CS                       | GPIO29                            | GPIO29                            | --   |
| LR2021 BUSY                     | GPIO30                            | GPIO30                            | --   |
| LR2021 RESET                    | GPIO31                            | GPIO31                            | --   |
| LR2021 IRQ（模块 DIO10）        | GPIO5                             | GPIO5                             | --   |
| 音频 I2S DOUT（P4 → 编解码器）  | GPIO9                             | GPIO9                             | --   |
| 音频 I2S DIN（编解码器 → P4）   | GPIO11                            | GPIO11                            | --   |
| 音频 I2S WS / BCLK / MCLK       | GPIO10 / GPIO12 / GPIO13          | GPIO10 / GPIO12 / GPIO13          | --   |
| LCD TE                          | GPIO33                            | GPIO33                            | --   |
| LCD 背光 PWM                    | GPIO34                            | GPIO34                            | --   |
| 触摸 INT                        | GPIO6                             | GPIO6                             | --   |
| AXP517 INT                      | GPIO4                             | GPIO4                             | --   |
| BOOT 按键                       | GPIO35                            | GPIO35                            | --   |
| 用户按键                        | GPIO36                            | GPIO36                            | --   |
| ESP32-C5 EN / RESET             | GPIO54                            | GPIO54                            | --   |
| SD 卡 CLK / CMD                 | GPIO43 / GPIO44                   | GPIO43 / GPIO44                   | --   |
| SD 卡 D0 / D1 / D2 / D3         | GPIO39 / GPIO40 / GPIO41 / GPIO42 | GPIO39 / GPIO40 / GPIO41 / GPIO42 | --   |
| ESP32-C5 SDIO CLK / CMD         | GPIO18 / GPIO19                   | GPIO18 / GPIO19                   | --   |
| ESP32-C5 SDIO D0 / D1 / D2 / D3 | GPIO14 / GPIO15 / GPIO16 / GPIO17 | GPIO14 / GPIO15 / GPIO16 / GPIO17 | --   |

### XL9555 扩展口（I2C 地址 `0x20`）

| 控制信号                           | Standard    | Round       |
| ---------------------------------- | ----------- | ----------- |
| 扬声器功放控制                     | XL9555 P0_0 | XL9555 P0_0 |
| SD 卡电源使能                      | XL9555 P0_1 | XL9555 P0_1 |
| ESP32-C5 / 音频 / LCD 共用电源使能 | XL9555 P0_2 | XL9555 P0_2 |
| LCD 复位                           | XL9555 P0_3 | XL9555 P0_3 |
| 触摸复位                           | XL9555 P0_4 | XL9555 P0_4 |
| LoRa 电源使能                      | XL9555 P0_5 | XL9555 P0_5 |

LCD 图像数据使用 MIPI-DSI，摄像头图像数据使用 MIPI-CSI；这些高速信号使用专用接口，未列为普通 GPIO。
连接器和完整硬件接线请查阅 `hardware/` 中对应版本的原理图。

## 项目结构

| 目录 / 文件                                     | 主要职责                                                                                                                                      |
| ----------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `CMakeLists.txt`、`build.ps1`                   | 根工程构建入口；选择板型和示例，隔离构建目录并生成合并固件。                                                                                  |
| `cmake/`                                        | 板型 / 示例选择、硬件兼容性检查，以及所选示例的主组件模板。                                                                                   |
| `main/CMakeLists.txt`、`main/examples/`         | 注册所选示例的源文件和依赖；包含单外设示例以及 `factory_standard`、`factory_round` 综合应用。                                                 |
| `components/bsp/t_panel_p4/`                    | 共用 BSP；`include/bsp/` 提供板级 API 和引脚定义，`src/` 负责供电时序、总线与外设初始化，`profiles/` 保存板型默认配置。                       |
| `components/peripheral_drivers_lib/`            | 芯片级驱动库；`include/pdl/` 定义接口，`src/drivers/` 实现显示、触摸、音频、电源等驱动，`port/esp_idf/` 适配 ESP-IDF 总线、延时和 GPIO 中断。 |
| `components/connectivity/esp32c5_connectivity/` | 基于 ESP-Hosted 和 esp_wifi_remote 管理 P4 与 C5 的 SDIO 通信、Wi-Fi、蓝牙控制器及连接恢复。                                                  |
| `components/lvgl_page_manager/`                 | LVGL 页面注册、切换和页面生命周期管理。                                                                                                       |
| `managed_components/`、`dependencies.lock`      | ESP-IDF Component Manager 下载的第三方依赖及其版本锁定记录。                                                                                  |
| `hardware/`、`doc/`                             | 开发板原理图、芯片 / 屏幕规格书及参考资料。                                                                                                   |
| `firmware/`                                     | 预编译固件，包括 ESP32-C5 的 ESP-Hosted 从机镜像。                                                                                            |
| `.vscode/`                                      | ESP-IDF 插件设置及构建、烧录、监视任务。                                                                                                      |
| `partitions.csv`、`build/`                      | Flash 分区布局；按板型和示例存放的构建产物、配置与缓存。                                                                                      |

## 支持的编译环境

- **ESP-IDF：5.5.3 及以上版本**；低于 5.5.3 的版本不支持。建议优先使用 5.5.3 作为构建基线。
- **目标芯片：`esp32p4`**。
- **命令行：已导出 ESP-IDF 环境的终端**；仓库提供的 `build.ps1` 和 VS Code 任务使用 PowerShell，运行前需加载对应 ESP-IDF 安装的 `export.ps1`。
- **图形化开发：VS Code + Espressif ESP-IDF 扩展**，同样需要配置 ESP-IDF 5.5.3 或更高版本。

可先运行 `idf.py --version` 确认当前终端实际使用的 ESP-IDF 版本，再选择下列构建方式。

## 三种编译方式
工程支持三种构建方式。三种方式都使用相同的根目录 `CMakeLists.txt`、
硬件选择、示例选择，以及按变体隔离的构建目录命名规则。

| 构建方式                 | 入口                     | 如何选择硬件/示例              | 构建目录                | 适用场景                  |
| ------------------------ | ------------------------ | ------------------------------ | ----------------------- | ------------------------- |
| VS Code ESP-IDF 构建按钮 | ESP-IDF `Build Project`  | 先编辑 `.vscode/settings.json` | 由 `settings.json` 指定 | 适用vscode idf 图形化插件 |
| ESP-IDF 终端             | `build.ps1`              | 通过命令行参数选择             | 自动生成                | 终端、脚本和 CI           |
| VS Code 任务             | `ESP-IDF: Build example` | 启动任务时选择硬件和示例       | 自动生成                | 经常切换不同变体          |

每个硬件版本和示例组合都有独立的 CMake/Ninja 缓存。
再次编译相同组合时会复用缓存；切换任意一个选择时会使用新的构建目录，不会使其他变体失效。

### 1. VS Code ESP-IDF 构建按钮
ESP-IDF 插件的 `Build Project` 按钮可以编译 `.vscode/settings.json` 中配置的变体。
需要编译例程，需要先选择硬件版本和示例目录，然后点击 `Build Project` 按钮。
例如，编译 `standard` 硬件的 `factory_standard` 示例：

```json
{
  "C_Cpp.intelliSenseEngine": "default",
  "idf.openOcdConfigs": [
    "board/esp32p4-builtin.cfg"
  ],
  "idf.customExtraVars": {
    "IDF_TARGET": "esp32p4",
    "IDF_COMPONENT_MANAGER": "1"
  },
  /* Select the hardware board to be compiled */
  "tPanelP4.hardwareVersion": "standard",
  // "tPanelP4.hardwareVersion": "round",
  // "tPanelP4.hardwareVersion": "rect",

  /* Select the example to be compiled */
  // "tPanelP4.example": "audio/loopback",
  // "tPanelP4.example": "audio/play_sd_mp3",
  // "tPanelP4.example": "audio/play_wav",
  // "tPanelP4.example": "axp517",
  // "tPanelP4.example": "camera",
  // "tPanelP4.example": "lcd_touch",
  // "tPanelP4.example": "lr2021",
  // "tPanelP4.example": "lvgl_fps_test",
  // "tPanelP4.example": "lvgl_lcd_touch",
  // "tPanelP4.example": "sd",
  // "tPanelP4.example": "sdio_esp32c5",
  // "tPanelP4.example": "usb_msc",
  // "tPanelP4.example": "xl9555",
  // "tPanelP4.example": "deep_sleep_baseline",
  "tPanelP4.example": "factory_standard",
  // "tPanelP4.example": "factory_round",
  // "tPanelP4.example": "factory_rect",

  "tPanelP4.buildDirectory": "${config:tPanelP4.hardwareVersion}-${config:tPanelP4.example}",
  "idf.buildPath": "${workspaceFolder}/build/${config:tPanelP4.buildDirectory}",
  "idf.buildPathWin": "${workspaceFolder}\\build\\${config:tPanelP4.buildDirectory}",
  "idf.cmakeCompilerArgs": [
    "-G",
    "Ninja",
    "-DPYTHON_DEPS_CHECKED=1",
    "-DESP_PLATFORM=1",
    "-DT_PANEL_P4_HARDWARE_VERSION=${config:tPanelP4.hardwareVersion}",
    "-DT_PANEL_P4_EXAMPLE=${config:tPanelP4.example}"
  ],
  "idf.portWin": "COM232",
}

```

在 Windows 下，`idf.buildPath` 和 `idf.buildPathWin` 必须保持一致。
构建目录名称也必须与硬件版本和示例目录匹配。修改后先执行一次
`ESP-IDF: Configure ESP-IDF extension`，然后点击 `Build Project`。

对应的直接 `idf.py` 命令如下：
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

### 2. ESP-IDF 终端
请在已经导出 ESP-IDF 环境的 PowerShell 终端中，从工程根目录执行以下命令：

命令格式： 
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action build
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action flash -Port <COMX>
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action flash-monitor -Port <COMX>
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action menuconfig
.\build.ps1 -HardwareVersion <board-name> -Example <example-name> -Action fullclean

```powershell
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action build
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action flash -Port COM159
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action flash-monitor -Port COM159
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action menuconfig
.\build.ps1 -HardwareVersion standard -Example factory_standard -Action fullclean
```

不要在 `main/examples/<name>` 目录中执行 `idf.py build`。请使用 `build.ps1`，或者使用下面所示的根工程 `idf.py` 等效命令。
编译输出会按照硬件版本和示例目录分别保存在 `build` 下的独立子目录中：

```text
build/standard-factory_standard/
```

正常编译会自动在对应构建目录生成 `T-Panel-P4-<版型>-<例程>-merged.bin`，
例如 `T-Panel-P4-standard-factory_standard-merged.bin`。例程路径中的 `/` 会替换为 `-`。
合并文件包含 bootloader、分区表和应用程序，可从地址 `0x0` 烧录。`T-Panel-P4-<example>.bin` 仍为
单独的应用程序镜像，其烧录地址以同目录下的 `flash_args` 为准。

### 3. VS Code 任务
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

## 添加示例
创建 `main/examples/<name>/CMakeLists.txt`，并在其中添加示例源文件和私有依赖：

```cmake
list(APPEND EXAMPLE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/main.cpp"
)
list(APPEND EXAMPLE_PRIV_REQUIRES
    peripheral_drivers_lib
    t_panel_p4
)
```

然后使用以下命令编译：

```powershell
.\build.ps1 -HardwareVersion standard -Example <name>
```

编译输出会写入 `build/standard-<name>`。
