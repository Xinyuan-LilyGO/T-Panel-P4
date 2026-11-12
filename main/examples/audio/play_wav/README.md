# Embedded WAV playback

This example uses the shared `t_panel_p4` BSP on standard, round, and rect
hardware to play `sample-3s.wav` repeatedly through ES8389. The bundled file
contains uncompressed 44.1 kHz, 16-bit stereo PCM.

`wav.hpp` validates the RIFF container, locates the `fmt ` and `data` chunks,
and skips optional metadata and padding. Only PCM data is passed to
`Board::writeAudio()`. The sample rate comes from the WAV header; capture is
disabled and output uses 384x MCLK at -12 dB. Replacing the asset requires
an uncompressed 16-bit stereo WAV file.

Playback enables the BSP output, writes PCM in chunks while accounting for
short writes, drains the final samples, and disables output before the next
iteration. Invalid files stop initialization. Playback errors disable output
and are logged before retrying after one second.

```powershell
.\build.ps1 -HardwareVersion standard -Example audio/play_wav -Action build
.\build.ps1 -HardwareVersion standard -Example audio/play_wav -Action flash-monitor -Port COM7
```

Use `round` or `rect` for the corresponding hardware. Output is stored in
`build/<hardware>-audio-play_wav`.

The parser regression test uses the bundled asset plus truncated, malformed,
unsupported-format, and metadata-bearing variants. Run from the repository
root with a host C++ compiler:

```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests/examples/wav_test.cpp -o build/wav_test.exe
.\build\wav_test.exe main/examples/audio/play_wav/sample-3s.wav
```
