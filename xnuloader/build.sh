#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
#
# usage: xnuloader/build.sh [device]
#
# With a device (common/devices/<name>.conf), the loader is built for that phone:
# the place where the boot shim leaves the payload is the device's own.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

DEVICE="${1:-}"
EXTRA=""
if [ -n "$DEVICE" ]; then
	PROFILE="$HERE/../common/devices/$DEVICE.conf"
	[ -f "$PROFILE" ] || { echo "build.sh: no profile for $DEVICE ($PROFILE)"; exit 1; }
	# shellcheck disable=SC1090
	PAYLOAD_BASE=$(sed -n 's/^PAYLOAD_BASE=\([^ ]*\).*/\1/p' "$PROFILE" | head -1)
	PAYLOAD_BASE=${PAYLOAD_BASE:-0xF0000000}
	EXTRA="-DPAYLOAD_BASE=$PAYLOAD_BASE"
	echo "build.sh: $DEVICE, payload at $PAYLOAD_BASE"
fi

clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter $EXTRA -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 /out:loader.efi loader.obj
ls -la loader.efi
