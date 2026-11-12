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

应用使用 `<bsp/board.hpp>` 和 `bsp::Board`；需要读取当前板型引脚时使用
`<bsp/pins.hpp>` 和 `bsp::pins`。板型专用类型仍保留在各自的命名空间中。

```cpp
#include <bsp/board.hpp>

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

`board.initialize()` 初始化整板。也可单独调用 `initializeDisplay()`、
`initializeCamera()` 等方法，BSP 会准备该外设所需的公共资源。
`board.devices()` 返回已初始化设备的指针快照；未初始化的设备返回空指针，
设备释放或重新初始化后应重新获取。

屏幕、触摸芯片和上电时序等硬件差异在对应外设源码中按板型选择。
Rect 没有 XL9555、LR2021 和 ESP32-C5，构建时会排除不适用的功能。

## 显示配置

三种板型共用 `bsp::DisplayConfig` 和 `Board::initializeDisplay(config)`。
无参调用使用相同的默认值：RGB888 输入、2 个帧缓冲、24 位面板输出。

| 参数 | 默认值 | 支持的值 |
| --- | --- | --- |
| `in_color_format` | `LCD_COLOR_FMT_RGB888` | RGB565、RGB666、RGB888 |
| `frame_buffer_count` | 2 | 1～3 |
| `bits_per_pixel` | 24 | 16、18、24 |
| `read_display_id` | standard/round：`true`；rect：`false` | 可选的面板 ID 诊断读取；rect 默认关闭，避免无响应时卡在 ESP-IDF DSI 接收等待中 |

`in_color_format` 决定帧缓冲及绘图数据的存储格式；`bits_per_pixel` 决定
面板 COLMOD 和 DSI 输出格式。两者独立配置，由 DPI bridge 处理格式转换。
RGB565 输入每像素占 2 字节，RGB888 占 3 字节；RGB666 按 ESP-IDF 的
18 位打包格式存储。LVGL、PPA 和应用填充的数据格式须与输入格式一致。
初始化表中的用户页 COLMOD 不会覆盖传入的面板位深。

```cpp
#include <bsp/board.hpp>

bsp::Board board;
bsp::DisplayConfig config{};
// 默认 RGB888；应用若生成 RGB565 数据，显式选择：
config.in_color_format = LCD_COLOR_FMT_RGB565;
config.frame_buffer_count = 2;
config.bits_per_pixel = 24;
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
#include <bsp/board.hpp>

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
#include <bsp/board.hpp>

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
| Rect | ES8389 DAC | ES7210 MIC1 / MIC2 |

默认格式为 48 kHz、16-bit、立体声、384× MCLK。ES8389 使用 7-bit I2C
地址 `0x10`，ES7210 使用 `0x40`。初始化包含 I2C 探测重试、提前开启 I2S
时钟、ES8389 ID 检查和 DVDD 复位回读检查；失败时关闭功放并释放部分资源。
Rect 的 ES8389 不提供录音，ADC/PGA 关闭、ASDOUT 置为高阻，录音增益和
静音由 ES7210 处理；ES7210 将统一的增益请求量化到它支持的硬件档位。

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
