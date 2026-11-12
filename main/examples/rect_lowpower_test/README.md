# Rect 外设低功耗测试

本示例使用统一 `t_panel_p4` BSP 的 rect profile，先初始化 ES8389/ES7210 音频、
SGM38121/OV2710 摄像头、SD 卡及 ILI9882 显示/触摸。DRV2605、QMC6309 和
QMI8658C 通过 BSP 的共享 I2C 总线直接初始化，QMI8658C 使用板上 SA0 低电平
对应的 7-bit 地址 `0x6A`。

显示输入显式设置为 RGB565，面板输出保持 24-bit。示例在 720×1440 屏幕绘制
五条色条并打开 GPIO23 背光，等待五秒后逐步执行休眠流程：触摸进入睡眠，
释放摄像头、音频及 SD 卡，关闭显示和功放，释放应用创建的 I2C 外设，最后
调用 `Board::enterDeepSleep()`。中间的等待用于观察各步骤的电流变化。
BSP 操作失败时返回错误，不会继续把失败步骤当作成功。

```powershell
.\build.ps1 -HardwareVersion rect -Example rect_lowpower_test -Action build
.\build.ps1 -HardwareVersion rect -Example rect_lowpower_test -Action flash-monitor -Port COM7
```

输出位于 `build/rect-rect_lowpower_test`。进入深睡眠后，通过 BSP 的
`pins::button` 低电平唤醒，ESP32-P4 从启动流程重新执行示例。本测试需要所有
上述外设正常连接以及可用的 SD 卡；只验证显示和触摸时请使用 `lcd_touch`。
