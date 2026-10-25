#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from prebuilt/uefi/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

Another device (common/devices/<name>.conf, read by common/build.sh) sets these in the environment:
  UEFI_TARGET   Mu-Silicium device name (default redwood)    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE                              addresses from its UEFI port's memory map
"""
import argparse, gzip, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(HERE, 'Mu-Silicium')
PREBUILT = os.path.join(HERE, 'prebuilt', 'uefi')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
FD_BASE, FD_SIZE = env_int('FD_BASE', 0x9FD00000), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
shim_dir = os.environ.get('BOOT_SHIM_DIR') or (
    os.path.join(MU, 'BootShim') if os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')) else '')
if shim_dir and os.path.isdir(shim_dir):
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'redwood.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
shim = open(parts['shim'], 'rb').read()
fd = open(parts['fd'], 'rb').read()
assert len(fd) == FD_SIZE, hex(len(fd))
if PAYLOAD_BASE == 0x80000000 and b'\xf0\x00\x00\x00\x00\x00\x00\x00' in fd:
    sys.exit(f'{parts["fd"]}: still contains XNU_PAYLOAD_BASE 0xF0000000; '
             'citrus needs 0x80000000 in SiliciumPkg .../XnuLoader.h and a rebuilt/patched SILICIUM_UEFI.fd')

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

# BootShim (REQUIRES_KERNEL_HEADER): image_size at offset 16 must cover shim + FD + XNUP.
# Poco ABL loads only that many bytes; a link-time FD_SIZE+shim leaves the payload unstaged.
if len(blob) > 24 and struct.unpack_from('<Q', blob, 8)[0] == FD_BASE:
    blob = blob[:16] + struct.pack('<Q', len(blob)) + blob[24:]

if os.environ.get('DTB'): parts['dtb'] = os.environ['DTB']
dtb = open(parts['dtb'], 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
# The boot image layout. The Poco M3's stock bootloader (and its siblings) refuses an image
# with header version 1 and the ramdisk at 0: it only accepts the layout of the older
# "sheng" reference board (header version 0, kernel at 0x8000, ramdisk at 1 MB, tags at 30 MB).
# BOOT_HEADER=1 (the default) is the old redwood layout; BOOT_HEADER=0 is the citrus one.
hdr = env_int('BOOT_HEADER', 1)
cmd = [sys.executable, parts['mkbootimg'], '--kernel', kernel, '--ramdisk', parts['ramdisk'], '-o', args.output]
if hdr == 0:
    # the layout the older "sheng" reference board uses
    cmd += ['--header_version', '0', '--base', '0x0', '--kernel_offset', '0x00008000',
            '--ramdisk_offset', '0x01000000', '--second_offset', '0x00000000',
            '--tags_offset', '0x01e00000', '--pagesize', '4096']
elif hdr == 2:
    # Header v2 with a 16 MB header slot; kernel file offset 0x1000000, load address 0x8000
    # (matches ~/citrus-port/mac-boot.img and MIUI boot.img).
    kernel_file_offset = 0x01000000
    kernel_len = os.path.getsize(kernel)
    ramdisk_offset = kernel_file_offset + ((kernel_len + 4095) // 4096) * 4096
    cmd += ['--header_version', '2', '--base', '0x0', '--kernel_offset', '0x8000',
            '--ramdisk_offset', hex(ramdisk_offset), '--second_offset', '0x00000000',
            '--tags_offset', '0x100', '--pagesize', '4096', '--os_version', '0x20000199',
            '--dtb', parts['dtb']]
else:
    cmd += ['--header_version', '1']
try:
    subprocess.check_call(cmd)
finally:
    os.unlink(kernel)

def _verify_citrus_boot(path):
    h = open(path, 'rb').read(48)
    if h[:8] != b'ANDROID!':
        sys.exit(f'{path}: not an Android boot image')
    ks, ka, _, _, _, _, _, ps = struct.unpack_from('<8I', h, 8)
    if ka != 0x8000 or ps != 4096:
        sys.exit(f'{path}: bad citrus layout (kernel_addr={ka:#x} page_size={ps}; want 0x8000 and 4096). '
                 f'Set BOOT_HEADER=0 in the device profile when calling mkxnuimg.py.')

if hdr in (0, 2):
    _verify_citrus_boot(args.output)

print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
