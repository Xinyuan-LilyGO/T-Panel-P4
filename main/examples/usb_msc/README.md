# USB MSC SD-card example

This example exposes the board's SD card to a computer as a USB Mass Storage
Class device. The BSP powers the card through XL9555 P0_1 and initializes the
four-bit SDMMC interface without mounting FatFS locally. The BSP also owns the
TinyUSB descriptors, MSC driver, raw block-device binding, and cleanup. The
example only calls `Board::initializeUsbMsc()`.

Build in its independent directory:

```powershell
.\build.ps1 -Example usb_msc -Action build
```

Flash the resulting image, insert a formatted SD card, and connect the board's
USB OTG/device port to the computer. The disk should appear as `T-Panel-P4 SD
Card`. Use the operating system's eject function before unplugging USB or
removing the SD card so pending writes are flushed.

The application must not mount or access the SD card while it is exposed to
the USB host. `Board::initializeUsbMsc()` enforces this by initializing the
card as an unmounted raw block device.
