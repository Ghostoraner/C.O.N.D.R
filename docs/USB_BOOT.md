# USB Boot

## Build

Install `xorriso`, then build the hybrid image:

```sh
make usb-iso
```

The output is `build/condr-usb.iso`. The build includes Limine's BIOS boot code,
an EFI boot image, and the hybrid partition metadata needed for direct USB
writing. The ordinary `make iso` target remains useful for optical media and
QEMU; when `xorriso` is unavailable it can fall back to `genisoimage`, which
does not produce the same guaranteed hybrid USB layout.

## Write on Linux

Identify the USB device first:

```sh
lsblk -o NAME,SIZE,MODEL,TRAN
```

Write the ISO to the whole device, not to a numbered partition. This erases the
selected device:

```sh
sudo dd if=build/condr-usb.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

Replace `/dev/sdX` with the correct device. Check the model and capacity again
before confirming the command. A graphical image writer can also write the ISO
in raw mode.

Boot the machine from the USB device and select its UEFI or legacy BIOS entry
in the firmware boot menu. Secure Boot is not supported by this experimental
kernel and may need to be disabled.