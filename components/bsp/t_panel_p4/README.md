# T-Panel-P4 BSP

Standard、Round 和 Rect 都使用 ESP-IDF 组件 `t_panel_p4`。在工程根目录用
`-HardwareVersion standard|round|rect` 选择板型；直接使用 CMake 时设置
`T_PANEL_P4_HARDWARE_VERSION`。示例的组件依赖统一写成 `t_panel_p4`。

| 要修改的内容 | 位置 |
| --- | --- |
| 整板初始化、I2C、XL9555 | [src/board.cpp](src/board.cpp) |
| 充电和供电、显示、触摸、摄像头、音频、SD、LoRa | [src/](src/)，按外设修改对应文件 |
| 板级引脚和接线常量 | [Standard](include/bsp/pins/standard.hpp) · [Round](include/bsp/pins/round.hpp) · [Rect](include/bsp/pins/rect.hpp) |
| SDK 默认配置与板型说明 | [profiles/](profiles/) |

应用使用 `<bsp/api/board.hpp>` 和 `bsp::Board`；需要读取当前板型引脚时使用
`<bsp/pins/pins.hpp>` 和 `bsp::pins`。板型专用类型仍保留在各自的命名空间中。

`Board` 的完整声明位于 `include/bsp/api/board.hpp`。Board 生命周期、总线和触摸
接口以及其他外设 API 都按实现文件拆分到同一层的 `include/bsp/api/*.hpp`；每个头文件
内部用注释和 `BSP_BOARD_API_PUBLIC` / `BSP_BOARD_API_PRIVATE` 区分公共成员、私有状态
和辅助函数。这样修改某个外设时，接口和实现可以按 `src/` 文件一一定位。

## 头文件分类

`include/bsp/` 的公共头文件保持直接可引用；`api/` 只用于按实现文件拆分
`Board` 的成员声明：

| 目录 | 内容 | 对应实现 |
| --- | --- | --- |
| `api/board.hpp`、`hardware.hpp` | Board 配置、设备视图、板型选择和编译期宏 | `src/board.cpp`、构建配置与所有 BSP |
| `pins/pins.hpp`、`pins/*.hpp` | 当前板型引脚分发和 Standard/Round/Rect 接线常量 | `src/board.cpp` 与各外设 `src/*.cpp` |
| `api/touch.hpp` | 三种触摸控制器的统一抽象 | `src/touch.cpp` |
| `api/slave_c5.hpp` | ESP32-C5 从机、Wi-Fi 和蓝牙控制器 | `src/slave_c5.cpp` |
| `api/*.hpp` | 按源文件拆分的 Board 成员声明；每个文件内部标注 public/private 区域 | 同名 `src/*.cpp` |

因此新代码直接使用 `<bsp/api/board.hpp>`、`<bsp/pins/pins.hpp>`、`<bsp/api/touch.hpp>`
和 `<bsp/api/slave_c5.hpp>`；这些路径就是实际头文件，不再经过重复目录转发。
`private_include/` 下的实现专用头文件也保持直接文件名，应用不应直接依赖这些文件。

```cpp
#include <bsp/api/board.hpp>

void app()
{
    static bsp::Board board;
    ESP_ERROR_CHECK(board.initializePower());  // 初始化 AXP517 及所需的板级资源
    if (auto *axp517 = board.devices().axp517)
    {
        // 通过 axp517 操作已初始化的驱动。
    }
}
```

`board.initialize()` 初始化整板；传入 `false`（`board.initialize(false)`）可跳过
SD 卡初始化，适合必须在无卡时启动的应用。也可单独调用 `initializeDisplay()`、
`initializeCamera()` 等方法，BSP 会准备该外设所需的公共资源。
`board.devices()` 返回已初始化设备的指针快照；未初始化的设备返回空指针，
设备释放或重新初始化后应重新获取。

屏幕、触摸芯片和上电时序等硬件差异在对应外设源码中按板型选择。
Rect 没有 XL9555、LR2021 和 ESP32-C5，构建时会排除不适用的功能。

## ESP32-C5 从机

Standard / Round BSP 内置 `bsp::Esp32c5Slave`，实现 ESP-Hosted SDIO
传输、从机身份和固件校验、Wi-Fi Station、Bluetooth Controller 以及传输
故障恢复。C5 物理供电和复位仍由 `bsp::Board` 管理；使用从机 API 前先调用
`board.initializePower()`。

项目随附的从机固件为
`firmware/slave-c5/esp32c5_slave_2.12.13.bin`，版本为 **ESP-Hosted 2.12.13**。
P4 主机 BSP 同样固定使用 `espressif/esp_hosted` 2.12.13 和
`espressif/esp_wifi_remote` 1.6.4。C5 固件至少应与主机保持相同的 ESP-Hosted
大版本；如果需要强制主机和从机版本完全一致，可设置：

```cpp
bsp::Esp32c5Config config{};
config.require_exact_hosted_version = true;

bsp::Board board;
ESP_ERROR_CHECK(board.initializePower());
bsp::Esp32c5Slave c5;
ESP_ERROR_CHECK(c5.initialize(config));
```

基本生命周期如下：

```cpp
bsp::Board board;
ESP_ERROR_CHECK(board.initializePower());

bsp::Esp32c5Config config{};
config.expected_sdio.clock_gpio = bsp::pins::sd2Clock;
config.expected_sdio.command_gpio = bsp::pins::sd2Command;
config.expected_sdio.data0_gpio = bsp::pins::sd2Data0;
config.expected_sdio.data1_gpio = bsp::pins::sd2Data1;
config.expected_sdio.data2_gpio = bsp::pins::sd2Data2;
config.expected_sdio.data3_gpio = bsp::pins::sd2Data3;
config.expected_sdio.enable_gpio = bsp::pins::esp32c5Enable;

bsp::Esp32c5Slave c5;
ESP_ERROR_CHECK(c5.initialize(config));
// c5.startWifiStation(...);

ESP_ERROR_CHECK(c5.deinitialize());
ESP_ERROR_CHECK(board.deinitialize());
```

`Esp32c5Slave` 仅在 Standard / Round 构建；Rect 没有该从机和对应 API。

## Rect 传感器

仅 Rect 提供 QMI8658C 六轴 IMU 和 QMC6309 三轴磁力计，BSP 实现在
`src/sensor.cpp`，复用 `peripheral_drivers_lib` 芯片驱动及 GPIO3/GPIO2 共享 I2C。
Standard / Round 不编译该文件，也不暴露传感器类型或接口。

`initializeSensors()` 初始化两个传感器，也可单独调用 `initializeQmi8658c()`
或 `initializeQmc6309()`。它们自动准备板级电源和 I2C；重复调用保留已有配置，
修改配置需先反初始化。组合初始化失败时只回滚本次新建的传感器。
Rect 的整板 `initialize()` / `isInitialized()` 包含这两个传感器。

| 配置 | 默认值 / 说明 |
| --- | --- |
| QMI8658C 地址 | 7-bit `0x6A`；无应答时尝试 `0x6B`，也支持指定 `0x6B` 为首选；`try_alternate_i2c_address=false` 固定地址，总线超时不回退 |
| QMC6309 地址 | 7-bit `0x7C` |
| I2C 速率 / 超时 | 两者均为 400 kHz / 100 ms；支持 1～400000 Hz、1～INT_MAX ms；超时为单次传输参数，初始化含复位等待和多次传输 |
| QMI8658C 测量 | 加速度 ±4 g、角速度 ±512 °/s，两者均为 125 Hz |
| QMC6309 测量 | 连续测量、±8 gauss、50 Hz、8 倍过采样；可通过 `config.qmc6309.sensor` 修改 |

```cpp
#if T_PANEL_P4_BSP_IS_RECT
// board 的生命周期应覆盖所有传感器操作。
bsp::SensorConfig config{};
config.qmi8658c.sensor.accelerometer_range = pdl::sensor::Qmi8658cAccelerometerRange::g_8;
ESP_ERROR_CHECK(board.initializeSensors(config));

// 在应用任务中周期轮询；尚无新数据时留待下次读取。
pdl::sensor::Qmi8658cDataReady ready{};
ESP_ERROR_CHECK(board.readQmi8658cDataReady(ready));
if (ready.accelerometer && ready.gyroscope) {
    pdl::sensor::Qmi8658cMeasurement imu{};
    ESP_ERROR_CHECK(board.readQmi8658cMeasurement(imu));
    // imu.acceleration_g、angular_rate_dps、temperature_c，以及原始值。
}

pdl::sensor::Qmc6309Status status{};
ESP_ERROR_CHECK(board.readQmc6309Status(status));
if (status.data_ready) {
    pdl::sensor::Qmc6309Measurement magnetic{};
    ESP_ERROR_CHECK(board.readQmc6309Measurement(magnetic));
    // magnetic.x_gauss / y_gauss / z_gauss；使用前检查 magnetic.overflow。
}
#endif
```

读取接口返回 `esp_err_t`；未初始化时返回 `ESP_ERR_INVALID_STATE`。
QMI 数据读取返回最新寄存器值，不等待 data-ready；QMC 无新数据时也返回
`ESP_ERR_INVALID_STATE`，因此建议先查询状态。数据沿用芯片坐标轴，BSP 不做
板级方向变换、磁场校准或姿态融合。默认采用轮询，不配置 GPIO49 中断。

`devices().qmi8658c` / `devices().qmc6309` 和同名 Board getter 返回借用驱动，
可用于身份读取、自检或其他底层操作。未初始化时为 `nullptr`，释放后须重新获取；
初始化、释放和驱动操作须由应用串行执行。通过 BSP 释放资源，避免直接调用
借用驱动的 `deinitialize()`。`deinitializeSensors()` 会先关闭 QMI 测量并禁用
振荡器、将 QMC 切到 suspend，再释放设备；即使其中一步失败也会尝试释放两者，
并返回首个错误。整板释放及 deep sleep 准备也执行该流程。

## Rect 马达

仅 Rect 提供 DRV2605 马达驱动，实现在 `src/motor.cpp`，芯片寄存器操作复用
`peripheral_drivers_lib`。使用 GPIO3/GPIO2 共享 I2C、固定 7-bit 地址 `0x5A`，
GPIO21 控制 EN。Standard / Round 不编译该文件，也不暴露马达接口。

`initializeMotor()` 自动准备板级电源和 I2C，预置 EN 低电平后配置输出，
拉高 EN 并等待芯片启动，再初始化驱动，最终停止输出并进入待机。
默认不会播放；显式启用 `config.driver.auto_calibrate` 时，初始化校准可能驱动马达。
重复初始化保留现有配置，修改配置需先调用 `deinitializeMotor()`。
初始化失败会释放设备并关闭 EN，公共电源和 I2C 保留供重试。

`bsp::MotorConfig` 的 `i2c_frequency_hz` 默认为 400000，允许 1～400000 Hz。
`driver` 沿用 PDL 的 `Drv2605Config`：默认 ERM、库 1、额定 3.0 V、钳位 3.3 V、
开环且不自动校准。配置应匹配连接的执行器；LRA 使用库 6，其电压寄存器计算
不同于 ERM。BSP 不根据马达接口推断执行器型号。

```cpp
#include <bsp/api/board.hpp>

#if T_PANEL_P4_BSP_IS_RECT
static bsp::Board board;
bsp::MotorConfig config{};
ESP_ERROR_CHECK(board.initializeMotor(config));
ESP_ERROR_CHECK(board.playMotorEffect(1)); // 非阻塞，效果 ID 为 1..117。

bool playing{};
ESP_ERROR_CHECK(board.readMotorPlaying(playing));
ESP_ERROR_CHECK(board.stopMotor());
ESP_ERROR_CHECK(board.deinitializeMotor());
#endif
```

| API | 行为 |
| --- | --- |
| `playMotorEffect(id)` | 停止旧输出，播放一个内置效果 |
| `playMotorSequence(effects, count)` | 播放 1～8 个效果，单项 ID 为 1～117；不接受等待编码或终止符 |
| `setMotorRealtimeValue(value)` | RTP 正向寄存器值 0～127；0 停止并待机，非零持续驱动；更新已有 RTP 时不经过待机 |
| `stopMotor()` | 清除 GO、将 RTP 清零，再进入待机；即使某项失败也尝试后续停止步骤 |
| `readMotorPlaying(playing)` | 内部触发模式查 GO，RTP 模式查输出值，待机返回 false；其他底层模式返回 `ESP_ERR_NOT_SUPPORTED` |
| `readMotorStatus(status)` | 读取 DRV2605 原始状态寄存器 |
| `drv2605()` / `devices().drv2605` | 借用初始化后的驱动；未就绪为 `nullptr` |

播放接口为非阻塞，RTP 不带自动停止定时器；应用控制持续时间并明确停止。
`readMotorPlaying()` 表示芯片驱动状态，不测量马达实际运动。
每次操作的 `timeout_ms` 默认为 100，允许 1～INT_MAX ms，表示单次 I2C 传输
超时，不是效果播放时长。无效参数不会中断当前播放；播放设置失败时尝试停止。

`deinitializeMotor()` 尝试停止、待机和释放传输，即使 I2C 操作失败也会拉低 EN
并释放 GPIO，由板上 EN 下拉保持关闭，返回首个错误。
Rect 的整板 `initialize()` / `isInitialized()` 包含马达，整板释放和 deep sleep
准备也关闭马达。Light sleep 不自动停止马达，应用可在休眠前调用 `stopMotor()`。
初始化、播放、查询、释放和借用驱动操作须由应用在任务上下文串行执行。
可运行示例见 [motor](../../../main/examples/motor/README.md)。

## 显示配置

三种板型共用 `bsp::DisplayConfig` 和 `Board::initializeDisplay(config)`。
无参调用使用相同的默认值：RGB888 输入、2 个帧缓冲、24 位面板输出。

| 参数 | 默认值 | 支持的值 |
| --- | --- | --- |
| `in_color_format` | `LCD_COLOR_FMT_RGB888` | RGB565、RGB666、RGB888 |
| `frame_buffer_count` | 2 | 1～3 |
| `bits_per_pixel` | 24 | 16、18、24 |
| `read_display_id` | standard/round：`true`；rect：`false` | 可选的面板诊断读取；rect 使用带超时的 ID 查询并记录 DSI 状态，其他板型使用驱动的 ID 读取 |

`in_color_format` 决定帧缓冲及绘图数据的存储格式；`bits_per_pixel` 决定
面板 COLMOD 和 DSI 输出格式。目标芯片为 ESP32-P4 v3 及以上时，两者可
独立配置，由 DPI bridge 处理格式转换。面向旧版芯片的构建（当前 rect
profile）要求输入、输出 RGB 格式一致，否则初始化返回 `ESP_ERR_NOT_SUPPORTED`。
例如 24 位面板输出必须配 RGB888 输入，不能配 RGB565 输入。
RGB565 输入每像素占 2 字节，RGB888 占 3 字节；RGB666 按 ESP-IDF 的
18 位打包格式存储。LVGL、PPA 和应用填充的数据格式须与输入格式一致。
初始化表中的用户页 COLMOD 不会覆盖传入的面板位深。

```cpp
#include <bsp/api/board.hpp>

bsp::Board board;
bsp::DisplayConfig config{};
// 默认 RGB888；应用若生成 RGB565 数据，显式选择：
config.in_color_format = LCD_COLOR_FMT_RGB565;
config.frame_buffer_count = 2;
config.bits_per_pixel = 16; // 输入/输出一致；v3 及以上芯片也可选择 24 位输出。
ESP_ERROR_CHECK(board.initializeDisplay(config));

// 三种板型返回同一个 ESP-IDF 接口类型，无需判断具体面板芯片。
esp_lcd_panel_handle_t panel = board.displayPanel();
// esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels);
// esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, context);
auto active_config = board.displayConfig();
```

`displayPanel()` 与 `board.devices().display_panel` 返回同一借用句柄，
未初始化时为 `nullptr`；释放、重新初始化或 light sleep 后须重新获取。
通过 `Board::deinitializeDisplay()` 释放资源，避免单独删除借用句柄。
`displayConfig()` 返回当前配置，未初始化时返回默认值。
重复初始化会复用已有资源；修改参数应先反初始化。Light sleep 唤醒后会恢复
完整显示配置，包括输入格式、面板位深、帧缓冲数量和 ID 读取选项。

扫描时序、像素时钟及 DSI lane 速率统一由 `src/board.cpp` 的
`detail::displayHardwareConfig()` 返回，使用 `DisplayHardwareConfig` 结构体，
内部通过硬件宏选择 Standard / Round / Rect 的参数；`src/display.cpp`
只消费该结构体并选择对应面板驱动。

## 触摸配置

三种板型共用 `bsp::TouchConfig`、`bsp::TouchController` 和 `bsp::TouchData`。
`touchController()` 与 `devices().touch` 返回同一个借用对象；读取触摸数据时
无需选择 Jadard、GT911 或 ILI9882 类型。

| 参数 | 默认值 / 校验 |
| --- | --- |
| `i2c_address` | Standard `0x68`；Round `0x5D`（也支持 `0x14`）；Rect `0x41`，均为 7-bit 地址 |
| `i2c_frequency_hz` | Standard 200 kHz；Round 100 kHz；Rect 400 kHz；允许 1～400000 Hz |
| `perform_hardware_reset` | `true`；Round 地址回退时会为备用地址再复位一次，其他板型只复位一次 |
| `perform_software_reset` | `false`；可显式开启，Round 不支持，开启会返回 `ESP_ERR_NOT_SUPPORTED` |
| `interrupt_trigger` | Standard/Round `GPIO_INTR_NEGEDGE`；Rect `GPIO_INTR_POSEDGE`，支持下降沿、上升沿、双边沿 |
| `interrupt_handler` / `interrupt_argument` | ISR 回调及参数，默认空，允许轮询读取 |
| `try_alternate_i2c_address` | `true`，仅 Round 有效；首选地址无应答时尝试另一个 GT911 地址一次，设为 `false` 可固定地址 |

非法地址、速率或中断触发方式在操作硬件前返回 `ESP_ERR_INVALID_ARG`。
地址配置不会改变芯片支持的地址范围。Round 在硬件复位时通过 INT 电平选择
GT911 地址；首选地址无应答时，默认重新复位并尝试备用地址。
关闭硬件复位时只探测地址，不改变 RST/INT，应用须保证芯片已启动。
成功后 `touchConfig().i2c_address` 返回实际使用的地址。

初始化先检查共享 I2C，仅在总线尚未初始化时创建，再准备供电和复位。
不主动复位共享 I2C 总线。Round 最多探测两个地址；总线超时等错误直接返回，
只有地址无应答才执行回退。找到应答地址后只创建一次触摸驱动。
INT 在初始化期间直接配置为中断输入，即使没有用户回调也可轮询。
`initializeTouchInterrupt(handler, argument, trigger)` 可在随后注册回调，复用
已配置的引脚；显式 `deinitializeTouchInterrupt()` 才会移除中断并释放该引脚。
ISR 中只能执行中断安全操作，例如唤醒任务，I2C 读取应在任务中进行。

Rect 在复位后先探测 `0x41`。ILI9882 的 ICE 芯片 ID 查询属于可选诊断，
失败时继续读取正常协议及固件版本；这两项必须成功才会返回初始化成功。
面板信息和报文格式查询不发送 `0xF6` 前导命令，旧版固件使用对应的查询长度。

```cpp
#include <bsp/api/board.hpp>

bsp::Board board;
bsp::TouchConfig config{};
config.i2c_frequency_hz = 100000;  // 根据实际接线设置；地址默认匹配当前板型。
// 如需中断通知，在初始化前设置 config.interrupt_handler / interrupt_argument。
ESP_ERROR_CHECK(board.initializeTouch(config));

bsp::TouchController *touch = board.touchController();
bsp::TouchData report{};
const pdl::Status status = touch->readTouchData(report);
if (pdl::isOk(status) && report.finger_count != 0) {
    const auto &point = report.points[0];
    // point.x / point.y / point.strength
}
```

统一数据最多容纳 10 个触点，芯片实际最大触点数保持不变。`strength` 为
16-bit，保留 GT911 完整压力值；`track_id` 在 GT911 上为芯片跟踪 ID，其他
芯片使用本次报告的触点索引；`edge_touch` 仅 Jadard 提供，其余为 `false`。
`busy` 表示没有新报告，不能当作松手；只有成功读取且 `finger_count == 0`
才表示松手。所有读取错误均保留传入的报告，成功读取会清除旧触点。

`touchConfig()` 返回当前配置。重复初始化复用已有对象并保留配置，修改配置
需先反初始化；light sleep 恢复完整配置，包括 I2C 地址、速率和中断回调。
释放、重新初始化或 light sleep 后须重新取得借用指针。芯片专用的 ID、固件
查询和电源模式可通过 `touch->native()` 访问，普通触摸读取使用统一接口。

## 电源配置

三种板型共用 `src/power.cpp` 的 AXP517 实现，电源 API 无需按板型分支。
`initializePower()` 使用 `PowerConfig{}`；传入配置可修改充电参数、测量、
检测、CHGLED 和中断。初始化会检查共享 I2C，总线尚未初始化时自动创建。
Standard / Round 还会准备 XL9555 并开启 C5/audio/LCD 电源。

| 参数 | 默认值 |
| --- | --- |
| `input_current_limit_ma` / `input_voltage_limit_mv` | 1500 mA / 4600 mV |
| `minimum_system_voltage_mv` | 3500 mV |
| `charge_voltage_mv` / `charge_current_ma` | 4200 mV / 512 mA |
| `precharge_current_ma` / `trickle_current_ma` / `termination_current_ma` | 128 / 64 / 128 mA |
| `enable_charge_termination` / `enable_charger` | 开启 |
| `enable_adc` / `enable_fuel_gauge` / `enable_battery_detection` | 开启 |
| `enable_bc12_detection` / `enable_type_c_detection` | 开启 |
| `enable_charge_led` / `charge_led_output_type` / `charge_led_mode` | 开启 / 开漏 / Type A |
| `charge_led_cfg_state` | 高阻，仅 CFG 模式使用 |
| `enable_interrupt` / `interrupt_mask` | 关闭 / `0xFFFFFFFFU` |

充电数值由 AXP517 驱动检查范围并按寄存器步进量化；部分数值会向下取整，
不支持的目标电压或超范围配置会返回错误。
重复调用初始化会复用已有控制器，保留原配置。应用新配置前先反初始化。

```cpp
#include <bsp/api/board.hpp>

bsp::Board board;
// 默认配置：ESP_ERROR_CHECK(board.initializePower());
bsp::T_PANEL_P4_BSP_NAMESPACE::PowerConfig config{};
config.charge_current_ma = 512;
config.enable_interrupt = false;
ESP_ERROR_CHECK(board.initializePower(config));

// CHGLED：默认 Type A，充电亮、充满/未充电灭，故障按芯片规则闪烁。
ESP_ERROR_CHECK(board.setChargeLedEnabled(true));
ESP_ERROR_CHECK(board.setChargeLedOutputType(
    pdl::charge::Axp517ChargeLedOutputType::open_drain));
ESP_ERROR_CHECK(board.setChargeLedMode(pdl::charge::Axp517ChargeLedMode::type_a));
pdl::charge::Axp517ChargeLedConfig led{};
ESP_ERROR_CHECK(board.readChargeLedConfig(led));

// 即使初始化时关闭，也可以在运行中开启中断。
ESP_ERROR_CHECK(board.setPowerInterruptEnabled(true));
bool irq_enabled = board.isPowerInterruptEnabled();
ESP_ERROR_CHECK(board.setPowerInterruptEnabled(false));
ESP_ERROR_CHECK(board.deinitializePower());
```

CHGLED 手动控制使用 `setChargeLedMode(Axp517ChargeLedMode::cfg)` 和
`setChargeLedCfgState(...)`，可设置高阻、1 Hz/4 Hz 闪烁及驱动电平。
`drive_high` 需要推挽输出。初始化会回读校验 CHGLED 配置。
`setChargerEnabled()` / `getChargerEnabled()` 在三个板型均可调用。

`setPowerInterruptEnabled()` 同时控制 AXP517 中断掩码和 ESP32 GPIO 中断，
开启时使用初始化配置的 `interrupt_mask` 并清除旧的待处理中断。
Standard / Round 使用 GPIO4，Rect 使用 GPIO5。通过 `setPowerEventCallback()`
注册事件回调，回调中允许切换中断；关闭时已经执行中的回调可能继续完成。
首次开启中断时创建事件任务，关闭后保留到反初始化，以支持再次开启。
`isPowerInterruptEnabled()` 查询 BSP 开关状态；直接写底层驱动寄存器会绕过该状态。

`deinitializePower()` 会停止中断任务，关闭 ADC、电量计（含低频模式）、
电池检测、BC1.2 和 Type-C 检测，再释放驱动及任务资源。共享 I2C 和充电器
保持可用；需要停止充电时先调用 `setChargerEnabled(false)`。
应用应串行执行初始化和反初始化，且不能在电源事件回调中反初始化电源。

## 摄像头配置

三种板型共用 `src/camera.cpp` 中的 OV2710、CSI 和 ISP 实现，统一使用
Round 的缓冲区保护策略：保护应用尚未释放的帧，不发布已被 DMA 重新用于
采集的缓冲区，并在成功停流后清理待领取帧及帧通知。

初始化时通过 `CameraConfig` 配置采集参数，三种板型均可通过
`board.cameraConfig()` 读取当前配置：

```cpp
bsp::T_PANEL_P4_BSP_NAMESPACE::CameraConfig config{};
config.mode = pdl::camera::Ov2710Mode::raw10_1920x1080_25fps;
config.input_data_color_type = CAM_CTLR_COLOR_RAW10;  // Must match the sensor mode.
config.output_data_color_type = CAM_CTLR_COLOR_RGB888;  // RGB565 (default) or RGB888.
config.capture_buffer_count = 3;  // 支持 2～3 个，默认 3 个
config.auto_exposure_target = 0x28;  // 按需调整；默认 0x30
config.isp_clock_hz = 160'000'000;
config.allow_non_dma_psram_fallback = true;
ESP_ERROR_CHECK(board.initializeCamera(config));
ESP_ERROR_CHECK(board.startCameraStreaming());
```

默认模式为 1920×1080、25 FPS，RAW10 经 ISP 转换为 RGB565。
`input_data_color_type` 必须匹配传感器模式；目前 OV2710 驱动仅提供 RAW10，
因此使用其他输入格式会返回 `ESP_ERR_INVALID_ARG`。
`output_data_color_type` 支持 `CAM_CTLR_COLOR_RGB565` 和
`CAM_CTLR_COLOR_RGB888`，CSI、ISP 和采集缓冲区大小会同步调整。
RGB565 每像素占 2 字节，RGB888 每像素占 3 字节。上述分辨率下三个采集
缓冲区分别占用约 11.9 MiB 和 17.8 MiB PSRAM；RGB565 设置为两个时约 7.9 MiB，但应用持帧期间
可能丢弃更多帧。使用 `acquireCameraFrame()` 后，应在 CPU、PPA 等消费者
完成读取后调用 `releaseCameraFrame()`。调整已初始化的摄像头参数应先释放
持有帧，再调用 `reconfigureCamera(config)`；重复调用 `initializeCamera()`
不会应用新参数。

输出格式改变后，应用必须同步调整 PPA/编码器等消费者的输入格式、图像大小
检查，以及直接显示该图像时使用的显示帧缓冲格式。传感器本身仍输出 RAW10，
RGB 格式由 ESP32-P4 ISP 生成。重新配置失败时的回滚及管线恢复会保留颜色参数。

## 音频配置

三种板型共用 `src/audio.cpp` 的 `esp_codec_dev` 驱动实现。板级文件仅描述
引脚和输入芯片组合（`pins::audioCaptureUsesEs7210`）：

| 板型 | 输出 | 输入 |
| --- | --- | --- |
| Standard / Round | ES8389 DAC | ES8389 ADC |
| Rect | ES8389 DAC | ES7210 MIC1 / MIC2，可选 MIC3 播放参考 |

默认格式为 48 kHz、16-bit、立体声、384× MCLK。ES8389 使用 7-bit I2C
地址 `0x10`，ES7210 使用 `0x40`。初始化包含 I2C 探测重试、提前开启 I2S
时钟、ES8389 ID 检查和 DVDD 复位回读检查；失败时关闭功放并释放部分资源。
Rect 的 ES8389 不提供录音，ADC/PGA 关闭、ASDOUT 置为高阻，录音增益和
静音由 ES7210 处理；ES7210 将统一的增益请求量化到它支持的硬件档位。

`AudioConfig::output_volume_db` 和 `setAudioVolumeDb()` 直接按 dB 设置 ES8389，
不经过 `esp_codec_dev` 的百分比音量曲线。0 dB 表示单位增益，设置范围为
-95.5～32 dB，实际值按芯片支持的档位量化。

Rect 可设置 `AudioConfig::enable_aec_reference=true`，同时采集 ES7210 的
MIC1、MIC2 和 MIC3 播放参考信号。该模式支持 16-bit、双声道播放、
`I2S_STD_SLOT_BOTH`，其他板型返回 `ESP_ERR_NOT_SUPPORTED`；必须同时开启
`enable_capture`。默认关闭，原有双麦采集格式不变。

开启后 RX 使用四个 16-bit TDM 时隙，仅接收前三个时隙，MIC4 不启用；TX
使用两个 32-bit 时隙传送原有双声道 16-bit PCM，共享每帧 64 个 BCLK。
`readAudio()` 返回交错的 `int16_t` 数据：`MIC1, MIC2, MIC3参考`，每帧 6 字节，
读取长度必须是 6 的整数倍。`audioCaptureChannelCount()` 返回当前采集通道数
（该模式为 3，关闭采集时为 0）。播放仍接收双声道 PCM，应先从三通道采集数据中
提取需要的麦克风通道，再交给播放器。

`aec_reference_gain` 独立设置 MIC3 增益，默认 0 dB；运行时调用
`setMicrophoneGain(gain, AudioMicrophoneChannel::reference)` 调整。
`AudioMicrophoneChannel::both` 仅控制 MIC1/MIC2。`no_dac_reference` 是 ES8389
自身的配置，不控制 Rect 的 MIC3 回采。BSP 只提供同步采样数据，AEC 算法由应用接入。
WAV 录音会保存三通道并写入对应文件头，录音块大小自动向下对齐到完整采样帧。
完整使用示例见 [Rect 音频说明](profiles/rect/README.md#mic3-aec-reference)。

`enable_speaker=true` 表示允许播放，初始化后功放仍保持关闭。直接输出 PCM
前调用 `setAudioOutputEnabled(true)`，它会清除残留 TX 数据、恢复 DAC 静音
状态、开启功放并等待 10 ms。正常播放结束后，先停止其他 TX 写入者，再调用
`drainAudio()`，最后关闭功放；取消或出错时先关闭功放。文件播放器自动管理
这些步骤。

```cpp
bsp::Board board;
bsp::T_PANEL_P4_BSP_NAMESPACE::AudioConfig config{};
config.output_volume_db = -12.0F;
ESP_ERROR_CHECK(board.initializeAudio(config));
ESP_ERROR_CHECK(board.setAudioOutputEnabled(true));
// board.writeAudio(...);
ESP_ERROR_CHECK(board.drainAudio());
ESP_ERROR_CHECK(board.setSpeakerEnabled(false));
```

`setAudioMuted()` 仅操作 ES8389 DAC，`setAudioCaptureMuted()` 操作实际输入
芯片的 ADC 并验证回读，成功读取的静音 PCM 仍会被清零。三种板型统一使用
`AudioWordLength`、`AudioMicrophoneGain`、`AudioMicrophoneChannel`。
`audioCodec()` 返回输出的 `esp_codec_dev_handle_t`；`audioInputCodec()` 返回
录音句柄（无输入通道时为空）。Standard / Round 的两个访问器返回同一设备，
Rect 则返回两个不同的设备。`devices().es8389` 在三种板型均为 BSP 的
`Es8389Device` 适配器；Rect 上应通过 `Board::setMicrophoneGain()` 设置 ES7210
增益。使用 Board API 调整音量和静音，以保持 BSP 状态一致。

音乐、录音回放和设置面板共享输出音量，暂时反初始化不会清除该音量；再次
初始化时应用传入的 `output_volume_db`。`deinitializeAudio()` 会先停止 BSP
录音与音乐任务，再释放输入、输出 codec 和 I2S 资源。应用自己的读写任务
应在重配置或释放前停止，并在同一板子上只保留一个音频资源所有者。

## 音乐和录音

Standard、Round、Rect 共用同一套音乐和录音 API 及实现，无需按板型分支。
音乐支持播放、暂停、继续、停止、进度跳转、下一首回调、音频电平、64 点波形、
当前路径以及 ID3v2.3/v2.4 封面和歌词读取。ID3 解析处理扩展头和文本编码，
歌词统一转换为 UTF-8；`getMusicMetadata()` 可一次读取封面和歌词。
进度跳转需要已知时长和底层 GMF 支持，字节位置按文件大小与时长比例估算。

编译启用重采样时，`playMusicFile()` 按
`CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE` 配置 I2S，而非源文件采样率。
未启用重采样时使用探测到的源采样率；无法探测时保留当前采样率。
`auto_configure_audio=false` 时不自动修改输出，若输出采样率与 I2S 不一致，
提前返回 `ESP_ERR_NOT_SUPPORTED`。未编译重采样时，`allow_resample=true`
同样保留当前 I2S 配置，已知源采样率不匹配会被拒绝；该参数不会在运行时
启用重采样。解码后才发现的源采样率不匹配通过正常播放错误事件报告。

播放器内部任务栈为 16 KiB，BSP 播放控制任务栈为 8 KiB，均使用 PSRAM。
录音采集和写入任务各使用 8 KiB PSRAM 栈；默认 PCM 环形缓冲区为 64 KiB，
两个数据块各 4 KiB，也明确分配在 PSRAM。环形缓冲区控制结构、任务 TCB、
信号量和 I2S DMA 缓冲区由驱动或 FreeRTOS 保留在内部 RAM。
录音数据块和环形缓冲区在创建任务前分配，分配失败返回 `ESP_ERR_NO_MEM`，
不会回退占用内部 RAM。正常停止和任务创建失败都会释放已分配资源。

三个 BSP profile 默认启用 PSRAM、启动初始化和外部任务栈。已有构建目录的
`sdkconfig` 不一定会应用新默认值，应确认 `CONFIG_SPIRAM=y`、
`CONFIG_SPIRAM_BOOT_INIT=y`、`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`。
录音 WAV 格式跟随当前 `AudioConfig`，录音本身不做重采样。
