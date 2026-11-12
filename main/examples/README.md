# T-Panel-P4 示例

示例统一依赖 `components/bsp/t_panel_p4`，应用入口为 `<bsp/board.hpp>` 和
`bsp::Board`，引脚从 `<bsp/pins.hpp>` 的 `bsp::pins` 读取。
通过 `-HardwareVersion` 选择硬件，BSP 自动选择显示、触摸、音频输入和供电路径。
完整配置及资源生命周期见 [BSP 说明](../../components/bsp/t_panel_p4/README.md)。

| 示例                 | standard | round | rect  | 功能                                    |
| -------------------- | :------: | :---: | :---: | --------------------------------------- |
| `axp517`             |    ✓     |   ✓   |   ✓   | 电源参数、CHGLED 回读和状态             |
| `xl9555`             |    ✓     |   ✓   |   —   | IO 扩展器测试                           |
| `lcd_touch`          |    ✓     |   ✓   |   ✓   | RGB888 色条与触摸中断                   |
| `lvgl_lcd_touch`     |    ✓     |   ✓   |   ✓   | RGB565 LVGL 触摸坐标                    |
| `lvgl_fps_test`      |    ✓     |   ✓   |   ✓   | LVGL 性能测试                           |
| `camera`             |    ✓     |   ✓   |   ✓   | OV2710、ISP 与按屏幕比例裁剪的 PPA 预览 |
| `audio/loopback`     |    ✓     |   ✓   |   ✓   | 双麦克风 PCM 回环                       |
| `audio/play_wav`     |    ✓     |   ✓   |   ✓   | 内嵌 WAV 的 PCM 播放                    |
| `audio/play_sd_mp3`  |    ✓     |   ✓   |   ✓   | SD 音乐扫描、循环播放和按键切歌         |
| `sd`                 |    ✓     |   ✓   |   ✓   | SD 卡文件读写                           |
| `usb_msc`            |    ✓     |   ✓   |   ✓   | SD 卡作为 USB 大容量存储                |
| `lr2021`             |    ✓     |   ✓   |   —   | LoRa 发包                               |
| `sdio_esp32c5`       |    ✓     |   ✓   |   —   | ESP32-C5 连接、Wi-Fi 与时间同步         |
| `factory_standard`   |    ✓     |   —   |   —   | Standard 工厂 UI                        |
| `factory_round`      |    —     |   ✓   |   —   | Round 工厂 UI                           |
| `rect_lowpower_test` |    —     |   —   |   ✓   | Rect 外设初始化、释放和深睡眠           |

表中表示支持的硬件组合；运行结果仍需在对应实板上验证。Rect 没有 XL9555、
LR2021 和 ESP32-C5，构建配置会拒绝不支持的组合。

在已执行 ESP-IDF 5.5.3 `export.ps1` 的 PowerShell 中，从工程根目录运行：

```powershell
.\build.ps1 -HardwareVersion standard -Example lcd_touch -Action build
.\build.ps1 -HardwareVersion round -Example factory_round -Action build
.\build.ps1 -HardwareVersion rect -Example camera -Action build
.\build.ps1 -HardwareVersion rect -Example camera -Action flash-monitor -Port COM7
```

输出位于 `build/<hardware>-<example>`，示例名中的 `/` 替换为 `-`，例如
`build/round-audio-play_wav`。每个硬件和示例组合使用独立目录。

## 使用 BSP 的约定

- 按需调用 `initializePower()`、`initializeDisplay(config)`、
  `initializeTouch(config)`、`initializeAudio(config)` 等接口，BSP 会准备依赖资源。
- `DisplayConfig` 默认 RGB888 输入、24-bit 面板输出、两个帧缓冲。
  LVGL/PPA 生成 RGB565 数据时必须显式设置 `in_color_format = LCD_COLOR_FMT_RGB565`。
  输入格式决定缓冲区字节数，面板输出位深独立配置。
- 使用 `displayPanel()`、`touchController()` 和 `devices()` 获取借用资源。
  BSP 负责释放；反初始化、重新配置或 light sleep 后应重新获取。
- 普通触摸读取统一使用 `bsp::TouchData`。`pdl::Status::busy` 表示没有新报告，
  保留上一次状态；成功读到零触点才表示松手。芯片专用诊断使用 `native()`。
- `writeAudio()` 接受原始 PCM，不能直接传入 WAV 文件头。音频格式与
  `AudioConfig` 保持一致。直接播放前启用 `setAudioOutputEnabled(true)`；
  正常结束后先 `drainAudio()`，再关闭输出，取消或出错时直接关闭输出。
- Standard/Round 使用 ES8389 录放音；Rect 使用 ES8389 播放、ES7210 采集。
  录音增益、静音和音量使用 Board API，由 BSP 选择实际芯片。
- SD 文件系统访问和 USB MSC 互斥。切换所有者前停止文件访问并释放当前模式。
- 休眠或重新配置前停止应用的外设读写任务；BSP 之外创建的 I2C 设备也要先释放。
