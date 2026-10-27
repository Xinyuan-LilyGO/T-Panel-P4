# XL9555 hardware test

This example uses the `t_panel_p4_standard` BSP to create the board I2C bus and
XL9555 driver, then tests the XL9555 at address `0x20` on SDA GPIO3 and SCL
GPIO2. The example obtains the already initialized driver through
`Board::ioExpander()` and leaves all resource ownership with the BSP.

The default test is non-destructive. It verifies communication, reads all
inputs, exercises individual and 16-bit output latches only on pins that are
currently inputs, tests polarity inversion, and reads the software-filtered
interrupt status. Original directions, output latches, and polarity settings
are restored before `Board::deinitialize()` releases the driver and I2C bus.

Physical high/low output testing is disabled by default because the repository
does not define which XL9555 pins are safe to drive. Set `kOutputTestMask` in
`main.cpp` only after checking the board schematic. The selected pins are
driven low, high, and low for 250 ms each before the original state is restored.

Build, flash, and monitor from an exported ESP-IDF PowerShell:

```powershell
.\build.ps1 xl9555 flash-monitor COM198
```

The build output is stored in `build/xl9555`.
