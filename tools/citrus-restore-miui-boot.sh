#!/bin/bash
# Restore MIUI fastboot images on citrus after a bad HackiOS boot flash.
# Needs: phone in stock fastboot (Vol Down + Power), USB connected.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
ROM="${CITRUS_ROM_IMAGES:-}"
for d in \
	"$HOME/.local/share/Trash/files/Poco M3/fastboot_rom/citrus_global_images_"*/images \
	prebuilt/citrus-miui-images \
	"$HOME/citrus-port/restore/images"; do
	[ -d "$d" ] && [ -f "$d/boot.img" ] && ROM=$d && break
done
[ -n "$ROM" ] || { echo "citrus-restore-miui-boot: no ROM images/ with boot.img"; exit 1; }
echo "citrus-restore-miui-boot: using $ROM"
fastboot devices | grep -q fastboot || { echo "no fastboot device"; exit 1; }
for part in vbmeta vbmeta_system dtbo boot; do
	[ -f "$ROM/$part.img" ] && fastboot flash "$part" "$ROM/$part.img"
done
echo "citrus-restore-miui-boot: unplug USB, then: fastboot reboot  (or long-press power)"
