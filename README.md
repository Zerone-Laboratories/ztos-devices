# ztos-devices

The ztOS device registry: what ztConflo knows about each phone. One folder per phone, `devices/<codename>/`, named by its postmarketOS codename (`oneplus-enchilada`):

- `device.toml`: the entry (schema below).
- `overlay/` (optional): files copied into the root filesystem as they are, last.
- `packages.txt` (optional): extra Arch packages, one or more per line, `#` comments.

Entries are not written by hand at first: `ztconflo build` writes a draft (`status = "untested"`) for every phone it builds through the postmarketOS fallback, and `ztconflo promote` moves it forward after real boots, through a pull request. Nothing is pushed to the main branch directly.

## Status

`untested` (built, never booted) -> `boots` (reaches ztMETRO, display and touch work) -> `tested` (the whole hardware checklist passes). Status only moves forward.

## Schema

```toml
[device]
codename = "oneplus-enchilada"
name = "OnePlus 6"
arch = "aarch64"
status = "untested"              # untested | boots | tested
origin = "pmos"                  # pmos | native

[source.pmos]                    # only when origin = "pmos"
pmaports_commit = "<sha>"
device_pkg = "device-oneplus-enchilada"
kernel_pkg = "linux-postmarketos-qcom-sdm845"
firmware_pkgs = ["firmware-oneplus-sdm845"]
category = "community"           # main | community | testing

[kernel]
mainline = true
version = "6.x.y"
artifacts_sha256 = "<one hash over the kernel, modules and dtb>"
soc = "sdm845"
gpu_driver = "freedreno"

[boot]                           # frozen from deviceinfo; changed only by `ztconflo refresh`
flash_method = "fastboot"        # fastboot | heimdall-bootimg | heimdall-isorec
bootimg_header_version = 0
kernel_offset = "0x00008000"
ramdisk_offset = "0x01000000"
tags_offset = "0x00000100"
base = "0x00000000"
second_offset = "0x00f00000"
pagesize = 4096
dtb = "qcom/sdm845-oneplus-enchilada"
cmdline = "console=ttyMSM0,115200"
append_dtb = true
qcdt = false
sparse = true
rootfs_target = "userdata"       # partition the root filesystem image goes to
defaulted = ["bootimg_header_version"]   # fields mkbootimg's defaults filled in: check them
# heimdall phones also have heimdall_partition_kernel and heimdall_partition_initfs

[userspace]
daemons = ["rmtfs", "pd-mapper", "tqftpserv"]   # systemd units to enable

[ztmetro]
scale = 2.5
vibrator = "auto"
rotation = 0

[verified]                       # written by `ztconflo promote`
date = ""
by = ""
checklist = {}
```

`origin = "native"` means a ztOS kernel package exists in the ztOS pacman repository and `[source.pmos]` is absent.
