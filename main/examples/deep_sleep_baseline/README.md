# 深度睡眠基线例程

支持 Standard、Round、Rect，通过构建参数选择对应 BSP 和引脚。程序仅初始化
电源控制，随后调用 BSP 的深度睡眠 API，不启动显示、音频、摄像头、SD 或 USB 应用。

Standard / Round 已完成编译验证。Rect 已做代码适配，本次按要求不继续编译验证；
首次配置曾因无法连接 ESP-IDF 组件仓库而停止，不能视为已编译通过。

## 执行流程

1. 打印唤醒原因，调用 `board.initializePower()` 初始化共享 I2C 和 AXP517。
2. Standard / Round 额外初始化 XL9555，并将全部 16 路设为输出低电平。
   Rect 没有 XL9555，电源控制使用 P4 GPIO。
3. 使用当前板型的 `DeepSleepConfig`，禁用 GPIO 和定时唤醒。
4. 调用 `board.enterDeepSleep()`。BSP 反初始化外设，关闭并验证 AXP517 辅助模块，
   Standard / Round 验证 XL9555 输出，释放 I2C，保持各板型的指定输出为低，
   配置 UART 睡眠隔离，最后由 ESP-IDF 进入深睡。

```cpp
bsp::T_PANEL_P4_BSP_NAMESPACE::DeepSleepConfig sleep_config{};
sleep_config.wakeup_gpio = GPIO_NUM_NC;
sleep_config.timer_wakeup_us = 0;
ESP_ERROR_CHECK(board.enterDeepSleep(sleep_config));
```

未配置唤醒源，深睡后需复位或重新上电。Flash / PSRAM 在运行期间保持供电，
由 ESP-IDF 深睡入口处理关断。芯片最低版本沿用所选 BSP：Standard / Round
当前为 rev 3.1，Rect 当前为 rev 1.0；例程不强制覆盖为 rev 3.1。

## 构建与烧录

在已导出 ESP-IDF 环境的 PowerShell 中运行：

```powershell
./build.ps1 -HardwareVersion standard -Example deep_sleep_baseline -Action build
./build.ps1 -HardwareVersion round -Example deep_sleep_baseline -Action build
./build.ps1 -HardwareVersion rect -Example deep_sleep_baseline -Action build
```

烧录时使用与实板一致的型号，例如：

```powershell
./build.ps1 -HardwareVersion round -Example deep_sleep_baseline -Action flash -Port COMx
```

三种板型分别使用 `build/standard-deep_sleep_baseline`、
`build/round-deep_sleep_baseline`、`build/rect-deep_sleep_baseline`，不共用构建缓存。
VS Code 使用 `.vscode/settings.json` 中的 `tPanelP4.hardwareVersion` 选择板型，
`tPanelP4.example` 设为 `deep_sleep_baseline`。

## 测量范围

这是 BSP 深睡基线，当前入口不执行此前的独立 LDO、USB、SGM38121 寄存器诊断函数。
不应以旧版诊断日志判断当前固件的实际电源状态。P4 核心 DCDC、四路 LDO、
常供电 VDD3V3 是不同支路，各电源是否关闭需在深睡后测量确认。

BSP 关闭 AXP517 ADC、电量计、电池检测、BC1.2 / Type-C 检测、指示灯和
VBUS 强制放电，保留 Buck 和充电策略。Standard / Round 使用 XL9555 关闭
共享外设电源；Rect 没有该共享电源开关，不能据此认为其所有外设都已断电。
未初始化的设备也不保证已收到自身的低功耗指令。

测量电池端电流时断开两处 USB 和外接串口，保持电池电压、接线和量程一致。
选用正确板型只保证使用对应 BSP，三种板型的整板电流需分别实测。

Standard V0.3 排查记录：关闭 AXP517 辅助模块约降低 0.7 mA；核心电源及
四路 LDO 实测接近 0 V 后，电池端仍约 14 mA，尚未定位到具体支路。
其 CH342F VIO 接常供电 VDD3V3，V3 / VDD5 实测为 0 V，不能据此认定
CH342F 消耗了全部 14 mA。原理图元件编号只适用于该版 Standard 板。
