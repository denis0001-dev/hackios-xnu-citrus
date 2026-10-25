#!/bin/bash
# Flash HackiOS USB loader boot image on POCO M3 (stock fastboot only).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
export XL_DEVICE=citrus
# shellcheck disable=SC1091
. common/devices/citrus.conf
export UEFI_TARGET DTB BOOT_HEADER FD_BASE FD_SIZE PAYLOAD_BASE UEFI_FD BOOT_SHIM_DIR
xnuloader/build.sh citrus
xnuloader/mkpayload.py -o usb-payload.bin loader.efi=xnuloader/loader.efi --usb
./mkxnuimg.py -p usb-payload.bin -o xnu-usb.img
avbtool make_vbmeta_image --padding_size 4096 --flags 3 --output /tmp/vbmeta_hackios.img
timeout 3 fastboot devices 2>/dev/null | grep -q fastboot || { echo "no fastboot device (timeout 3s)"; exit 1; }
fastboot flash vbmeta /tmp/vbmeta_hackios.img
fastboot flash boot xnu-usb.img
fastboot reboot
echo "citrus-flash-hackios-boot: check: timeout 3 fastboot getvar product  (want xnuloader)"
