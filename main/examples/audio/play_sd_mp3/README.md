# SD 卡 MP3 播放示例

本示例用于验证 `t_panel_p4_standard` BSP 的音频播放接口。它初始化
T-Panel-P4 Standard、挂载 SD 卡，然后依次播放 `/music` 目录中的三首
MP3。示例是 BSP 驱动使用和回归测试，不包含播放列表 UI；播放顺序和测试
逻辑写在 `main.cpp` 中，实际产品应在应用或音乐服务层管理播放列表。

## SD 卡文件

请将 SD 卡格式化为 BSP 支持的 FAT 文件系统，并创建下面的目录和文件。路径
大小写、空格、连字符以及 `.mp3` 扩展名都必须一致：

```text
/music/Fever Pitch - Richz.mp3
/music/lilygo.mp3
/music/Taylor Swift - Love Story.mp3
```

程序挂载点是 `/sdcard`，因此运行时访问的完整路径为：

```text
/sdcard/music/Fever Pitch - Richz.mp3
/sdcard/music/lilygo.mp3
/sdcard/music/Taylor Swift - Love Story.mp3
```

示例文件的预期格式如下，最终格式和时长以启动时输出的 `music info` 为准：

| 文件 | 预期格式 |
| --- | --- |
| `Fever Pitch - Richz.mp3` | 44.1 kHz，立体声 |
| `lilygo.mp3` | 16 kHz，单声道 |
| `Taylor Swift - Love Story.mp3` | 44.1 kHz，立体声 |

## 长文件名配置

两个测试文件包含空格，示例的 `sdkconfig.defaults` 已启用：

```text
CONFIG_FATFS_LFN_HEAP=y
```

如果之前已经生成过该示例的构建目录，旧的 `CONFIG_FATFS_LFN_NONE=1` 可能仍
保存在生成的 `sdkconfig` 中。修改配置后必须执行一次 `fullclean`，不能只运行
普通 `build`。

## 构建、烧录和运行

请在已经执行 ESP-IDF 5.5.3 `export.ps1` 的 PowerShell 中，从工程根目录执行：

```powershell
.\build.ps1 -Example audio/play_sd_mp3 -Action fullclean
.\build.ps1 -Example audio/play_sd_mp3 -Action build
.\build.ps1 -Example audio/play_sd_mp3 -Action flash-monitor -Port COM198
```

将 `COM198` 替换为实际串口。也可以分开执行：

```powershell
.\build.ps1 -Example audio/play_sd_mp3 -Action flash -Port COM198
.\build.ps1 -Example audio/play_sd_mp3 -Action monitor -Port COM198
```

上电后示例会初始化音频、检查三个文件是否存在，然后按顺序播放。任一文件
缺失时，程序会打印 `/sdcard/music` 的实际目录内容，并以
`ESP_ERR_NOT_FOUND` 结束，便于检查文件名和 FATFS 长文件名配置。

## 本示例验证的 BSP 功能

示例直接调用 `playMusicFile(path, config)` 播放。BSP 会在播放命令内部完成一次
格式探测，并通过事件回调报告编码器、采样率、声道、比特率和时长。应用只有在
需要播放前预览元数据时才需要单独调用 `probeMusicFile()`。示例使用固定 48 kHz
音频输出，输入文件由 GMF 重采样到固定输出，配置为：

```cpp
config.auto_configure_audio = false;
config.allow_resample = true;
config.reject_unknown_format = true;
```

这表示 BSP 不会在歌曲切换时重建 ES8389/I2S；播放器把不同采样率的输入
重采样到 48 kHz，再经过声道和位深转换输出为 16-bit、双声道 PCM。未知格式
仍会被拒绝。示例配置中显式启用采样率转换：

```text
CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN=y
CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE=48000
```

播放器运行时会在三首歌曲之间复用，歌曲切换不会调用
`reconfigureAudio()`。

示例还会验证并打印：

- SD 卡挂载和三个文件存在性检查。
- `setMusicEventCallback()` 的状态变化、格式信息和错误回调。
- `musicFormat()`、`musicDurationMs()`、`musicPositionMs()` 的信息更新。
- 第一首歌的 `pauseMusic(true)` / `pauseMusic(false)` 暂停和恢复。
- 第一首歌的 `fadeMusicVolumePercent()` 音量淡入淡出命令。
- `musicStatistics()` 中的输出字节数、输出调用次数、短写、错误、超时和任务栈余量。
- `seekMusicMs(1000)` 明确返回 `ESP_ERR_NOT_SUPPORTED`。

`seekMusicMs()` 当前不会伪造进度。ESP Audio Simple Player 没有向 BSP 暴露可用
的 seek 原语，因此返回不支持是预期行为。真正的 seek 需要底层播放器提供按帧
定位能力后再实现。

暂停和信息查询的最小用法如下：

```cpp
ESP_ERROR_CHECK(board.pauseMusic(true));   // 暂停并等待命令完成
const auto state = board.musicState();
const auto position_ms = board.musicPositionMs();
const auto duration_ms = board.musicDurationMs();
ESP_ERROR_CHECK(board.pauseMusic(false));  // 恢复并等待命令完成
```

播放事件回调应保持简短，只用于记录或通知应用任务；不要在回调中递归调用
播放器控制接口。播放列表、循环播放、上一首/下一首等策略不属于 BSP。

## 预期日志

不同 MP3 的实际时长和比特率会不同，以下只列出关键日志类型：

```text
SD card mounted at /sdcard
track 1/3: /sdcard/music/Fever Pitch - Richz.mp3, starting fixed-output playback
music info: ... Hz, ... ch, ... bit, duration=... ms
fixed audio output: 48000 Hz; resampling=enabled
pause/resume probe
volume fade probe
seek probe: ESP_ERR_NOT_SUPPORTED
track finished: duration=... ms, output=... bytes, ...
track 2/3: /sdcard/music/lilygo.mp3, probing media format
track 3/3: /sdcard/music/Taylor Swift - Love Story.mp3, probing media format
all 3 tracks completed
play_sd_mp3 example completed
```

## 常见问题

### `audio file not found` / `ESP_ERR_NOT_FOUND`

1. 确认文件位于 SD 卡根目录的 `music` 文件夹，而不是工程目录或其他目录。
2. 检查大小写、空格、连字符和扩展名是否完全一致。
3. 查看错误前打印的 `/sdcard/music` 目录列表。
4. 执行 `fullclean` 后重新构建，确认生成配置没有残留
   `CONFIG_FATFS_LFN_NONE=1`。

### 重采样没有生效

确认生成配置中存在 `CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN=y` 和
`CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE=48000`，并执行一次
`fullclean`。同时确认 BSP 音频初始化使用 48 kHz。

### `seekMusicMs()` 返回 `ESP_ERR_NOT_SUPPORTED`

这是当前 BSP 的明确限制，不表示 SD 卡或 MP3 文件损坏。暂停、恢复、停止和
实时进度查询仍然可用。

### 播放中止或输出超时

查看 `music error` 和 `track finished` 日志中的错误码及统计信息，重点关注
`output_short_writes`、`output_errors`、`playback_timeouts` 和任务栈余量。若
任务栈余量持续过低，应在 BSP 配置中增加播放器任务栈，而不是在应用循环中
反复重启播放。
