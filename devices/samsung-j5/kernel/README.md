# Galaxy J5 kernel modules

The prebuilt modules in `overlay/usr/lib/modules/6.12.1-msm8916/` are built from
these sources, for postmarketOS's `linux-postmarketos-qcom-msm8916` 6.12.1 (clang, kCFI).
Mainline has no SM5703 drivers.

- `sm5703_fuelgauge.c`: the battery (`/sys/class/power_supply/battery`) from the
  SM5703 fuel gauge (blsp_i2c4, 0x71), and `usb` online from the MUIC (extcon).
- `sm5703_vibrator.c`: the motor, which is on the SM5703's LDO2. Exposed as an input
  force-feedback device, which is what ztMETRO looks for.
- `panel-ams497hy01-backlight.patch`: brightness for the panel, as a backlight device, and
  display off sent while video still runs (no lines on the way down). Brightness
  works through AID (the share of each frame the OLEDs are dark), so the panel's
  calibrated gamma is left alone. ztconflo also puts this module into the reused
  initramfs, which loads the panel early.

device.toml's `dtb_set` adds the nodes these bind to.

## Rebuilding

Use the kernel's tag (`v6.12.1-msm8916` from github.com/msm8916-mainline/linux),
with the phone's `/proc/config.gz` as `.config`:

    make ARCH=arm64 LLVM=1 olddefconfig
    make ARCH=arm64 LLVM=1 HOSTCFLAGS=-Wno-error=incompatible-pointer-types-discards-qualifiers modules_prepare
    make ARCH=arm64 LLVM=1 KBUILD_MODPOST_WARN=1 M=$PWD/path/to/kernel modules

Keep `DEBUG_INFO_BTF_MODULES` as the config has it. Turning it off changes
`struct module`, and the kernel then refuses to load the module.
For the panel, apply the patch to the tree, then build `drivers/gpu/drm/panel/msm8916-generated`
the same way.
