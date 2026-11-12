# Two-microphone loopback

This example uses the shared `t_panel_p4` BSP on standard, round, and rect
hardware for 48 kHz, 16-bit stereo audio with 384x MCLK. The BSP captures MIC1
and MIC2 through ES8389 on standard/round or ES7210 on rect, then writes the
PCM unchanged to the ES8389 DAC and speaker output.

The speaker starts at `-24 dB`. Keep the microphones away from the speaker
during initial testing to avoid acoustic feedback. The monitor prints each
microphone channel's raw peak once per second; a 16-bit full-scale peak is
32768.

```powershell
.\build.ps1 -HardwareVersion standard -Example audio/loopback -Action build
.\build.ps1 -HardwareVersion standard -Example audio/loopback -Action flash-monitor -Port COM198
```

Select `round` or `rect` for the corresponding board. Output is stored in
`build/<hardware>-audio-loopback`.
