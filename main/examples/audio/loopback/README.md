# ES8389 two-microphone loopback

This example configures the standard T-Panel-P4 for 48 kHz, 16-bit stereo
audio. ES8389 ADC channels MIC1 and MIC2 are captured through I2S0 and written
back unchanged to the ES8389 DAC and speaker output.

The speaker starts at `-24 dB`. Keep the microphones away from the speaker
during initial testing to avoid acoustic feedback. The monitor prints each
microphone channel's raw peak once per second; a 16-bit full-scale peak is
32768.

```powershell
.\build.ps1 -Example audio/loopback -Action build
.\build.ps1 -Example audio/loopback -Action flash-monitor -Port COM198
```
