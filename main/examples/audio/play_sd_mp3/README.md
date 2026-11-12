# SD 卡 MP3 播放示例

本示例使用统一的 `t_panel_p4` BSP，支持 standard、round、rect。程序挂载
SD 卡后扫描 `/sdcard/music` 中的 MP3 文件（扩展名不区分大小写），最多读取
64 首，按文件名字节顺序排序并循环播放。目录中的文件名可以自定义。

## 准备与运行

在 FAT 格式的 SD 卡根目录创建 `music` 文件夹并放入至少一首 MP3。示例启用
`CONFIG_FATFS_LFN_HEAP=y`，支持长文件名。它不会在挂载失败时格式化 SD 卡。

在已导出 ESP-IDF 5.5.3 环境的 PowerShell 中执行：

```powershell
.\build.ps1 -HardwareVersion standard -Example audio/play_sd_mp3 -Action build
.\build.ps1 -HardwareVersion standard -Example audio/play_sd_mp3 -Action flash-monitor -Port COM7
```

按硬件选择 `standard`、`round` 或 `rect`，将 `COM7` 替换为实际串口。
输出目录为 `build/<hardware>-audio-play_sd_mp3`。

## 播放行为

- 使用 `initializeSdCard()` 和 `initializeAudio()` 按需初始化，由 BSP 管理资源。
- ES8389 输出固定为 48 kHz、16-bit、立体声、384× MCLK、-12 dB，关闭录音。
- `playMusicFile()` 使用 `auto_configure_audio=false`、`allow_resample=true`、
  `reject_unknown_format=true`。示例 SDK 配置显式启用 GMF 重采样到 48 kHz，
  BSP 负责启停输出和排空 DMA；歌曲切换复用播放器和音频资源。
- 使用 BSP 当前板型的 `pins::button` 切换上一首、`pins::boot_button` 切换下一首。
  按键约每秒采样一次；切歌前等待 `stopMusic(2000)` 完成。
- `getMusicLyrics()` 尝试读取歌词，存在时打印 UTF-8 文本长度。
  事件回调打印格式信息和播放错误，主循环每秒打印播放位置与时长。
- 正常播放结束后自动进入下一首；最后一首结束后回到第一首。

没有 MP3 或目录不存在时，扫描返回 `ESP_ERR_NOT_FOUND`；播放失败时记录错误并
结束循环。短按可能被一秒的按键采样周期漏过，测试时可按住直到切歌。

## BSP 接口约定

`allow_resample=true` 不会在运行时启用重采样，仍需编译配置支持。若修改目标
采样率，必须同时修改音频输出配置；已有构建目录还应检查生成的 `sdkconfig`。

此示例不主动执行暂停、淡入淡出或 seek 测试。产品可通过 BSP 的
`pauseMusic()`、`fadeMusicVolumePercent()`、`seekMusicMs()` 等接口扩展控制。
Seek 需要已知时长及底层 GMF 支持，不能假定任意文件都可定位。
播放事件回调应只记录或通知应用任务，不要在回调中递归调用播放器控制接口。
