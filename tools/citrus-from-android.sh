#!/bin/bash
# Android (USB debugging on) -> stock fastboot -> HackiOS loader boot image.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

adb wait-for-device
state=$(adb get-state 2>/dev/null || true)
[ "$state" = device ] || { echo "citrus-from-android: adb state=$state"; exit 1; }

adb reboot bootloader
for _ in $(seq 1 45); do
  if timeout 2 fastboot getvar product 2>&1 | grep -q 'product:'; then
    exec "$ROOT/tools/citrus-flash-hackios-boot.sh"
  fi
done
echo "citrus-from-android: fastboot did not appear (45 x 2s timeouts)"
exit 1
