# POCO M3 (citrus) — booting the USB loader

Prerequisites: phone in **stock fastboot** (power off, Vol Down + Power), unlocked bootloader.

Build citrus UEFI once (`python3 Mu-Silicium/build_uefi.py -d citrus -r RELEASE`) with
**`XNU_PAYLOAD_BASE = 0x80000000`** in `Silicon/.../XnuLoader.h` (redwood's `0xF0000000` is wrong
on citrus). Rebuild after `uefi/files/.../XnuLoader.c` changes (pool copy before `LoadImage`).
If Silicium shows `error image at … start failed: Not Found`, rebuild the FD — a one-byte patch is
not enough without the updated BootManager.

```bash
export XL_DEVICE=citrus
cd hackios-xnu-kernel
. common/devices/citrus.conf
export UEFI_TARGET DTB BOOT_HEADER FD_BASE FD_SIZE PAYLOAD_BASE UEFI_FD BOOT_SHIM_DIR

xnuloader/build.sh citrus
xnuloader/mkpayload.py -o usb-payload.bin loader.efi=xnuloader/loader.efi --usb
./mkxnuimg.py -p usb-payload.bin -o xnu-usb.img

avbtool make_vbmeta_image --padding_size 4096 --flags 3 --output /tmp/vbmeta_custom.img
fastboot flash vbmeta /tmp/vbmeta_custom.img
fastboot flash boot xnu-usb.img
fastboot reboot
```

Success: `timeout 3 fastboot getvar product` shows **`product: xnuloader`** (not `citrus`).

If it still says `citrus` after `flash boot`, the image layout or vbmeta chain is wrong — do **not** use `out/citrus-boot.img` from an old build (must pass `mkxnuimg` citrus verify: `kernel_addr=0x8000`, `page_size=4096`). Restore Android: `./tools/citrus-restore-miui-boot.sh`.

`fastboot boot` is **not** supported on this phone — only `flash boot`.

Restore Android: `fastboot flash boot ~/citrus-port/backup/boot-last-working.img`

After the loader is up: `./xl ramdisk userland/system.apfs` then `./xl run` with `XL_CMDLINE` for GPU (`cpus=3 adreno=15`, then `gpu=1`).
