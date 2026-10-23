# Porting HackiOS to another device

HackiOS was brought up on the POCO X5 Pro 5G (Snapdragon 778G). A Qualcomm phone with an unlockable bootloader
is the realistic next target: the boot chain (Mu-Silicium UEFI started with `fastboot boot`) and most of the kernel's
platform code carry over. Start with:

```sh
common/new-device.sh <codename> path/to/device.dtb
```

It writes `common/devices/<codename>.conf` (the profile `common/build.sh` and `common/run.sh` read) and
`common/devices/<codename>.platform.c` (the loader's description of the SoC), filling in what the device tree says.
Get the device tree from the phone's `vendor_boot` image, or from a running Android: `adb pull /sys/firmware/fdt`.

## 1. The UEFI

The phone needs a UEFI that our loader can run under. Port [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
the way `uefi/` ports redwood (it follows `lisa`, the same SoC package):

- `Platforms/<Vendor>/<codename>Pkg`: memory map and driver list (copy the closest device of the same SoC);
- `Binaries/<codename>`: the UEFI drivers of the phone's own firmware, extracted with
  [UEFIReader](https://github.com/WOA-Project/UEFIReader) from its `xbl.img`;
- apply our two changes from `uefi/mu-silicium.patch`: the boot shim copies the "XNUP" container after the UEFI
  image to `PAYLOAD_BASE`, and `BootManagerLib` starts the XNU loader from it instead of the menu.

Put the addresses in the profile: `FD_BASE`, `FD_SIZE` (where the shim puts the UEFI image) and `PAYLOAD_BASE`
(free RAM for the kernel and images). The prebuilt `BootShim.bin` is built for redwood's addresses: build the
shim from your Mu-Silicium tree (`mkxnuimg.py` does it when `Mu-Silicium/` is there).

## 2. The loader

`xnuloader/loader.c` describes each device with a `struct platform` and picks it by the framebuffer base the UEFI
reports. Paste `common/devices/<codename>.platform.c` next to `platform_redwood` and add it to `platforms[]`:

| Field | Where it comes from |
|---|---|
| `io_base`, `io_size` | one window that covers the SoC's peripherals; it must not start at 0 (XNU then skips clock setup) |
| `gic_dist`, `gic_redist` | the `arm,gic-v3` node's `reg` (filled in by `new-device.sh`) |
| `cores`, `core_shift` | the `cpu@` nodes; Qualcomm numbers cores in MPIDR's second affinity field (shift 8) |
| `small_cores` | a bit per core of the little cluster (lowest `capacity-dmips-mhz`) |
| `psci_hvc` | 1 if the firmware's PSCI is called with HVC (QEMU), 0 for SMC (phones) |
| `fb_base` | the framebuffer address; the loader prints it at start (`display W x H at ...`) |

Rebuild with `xnuloader/build.sh`; QEMU must still pick `qemu-virt` (no match = ramfb).

## 3. The kernel

What is generic: GICv3, the timer, PSCI SMP, the display (the UEFI's framebuffer), APFS and ramfs, the driver
replacements, the return-to-loader path. What is redwood-specific and needs looking at:

- **touch**: `bsd/dev/redwood_goodix.c` (Goodix GT9916 over bit-banged SPI) — another panel needs its own driver
  feeding `/dev/redwood-touch` (`bsd/dev/redwood_input.c`);
- **buttons, CPU frequency, watchdog**: GPIO/PMIC numbers and the EPSS block in `redwood_input.c`,
  `redwood_cpufreq.c`, `osfmk/arm64/loader_return.c`;
- **GPU / Wi-Fi / sound**: `redwood_adreno.c` (Adreno 642L) and `citrus_adreno.c` (Adreno 610), `redwood_wifi.c`
  (WCN6750 + ath11k), `redwood_audio.c` — the same Qualcomm subsystems (SMEM, SMP2P, GLINK, QRTR, QMI) are reused,
  the addresses and firmware differ. GPU details below.

Start with no extras: the system boots to the lock screen with only the framebuffer, touch and the CPU.

### GPU (Adreno 642L vs Adreno 610)

Redwood (POCO X5 Pro) has an Adreno 642L with a real GMU. Citrus (POCO M3, SM6115) has an Adreno 610 with a GMU
wrapper and no GMU firmware. `gpu=1` picks a path from what is in `fw/`: `a660_gmu.bin` → redwood; `a610_zap.mdt`
without that GMU image → citrus. `adreno_plat=redwood|citrus` overrides that when both trees are present.

| | redwood (Adreno 642L / a660) | citrus (Adreno 610) |
|---|---|---|
| driver | `bsd/dev/redwood_adreno.c` | `bsd/dev/citrus_adreno.c` |
| power | real GMU (`a660_gmu.bin`, HFI) | GMU wrapper at `0x0596a000`; CPU-owned GDSCs/clocks; no GMU firmware |
| MMIO | `0x03d*` (3D core `0x03d00000`) | `0x059*` (3D core `0x05900000`, SMMU `0x059a0000`) |
| chip id | `0x06030500` | `0x06000600` |
| GMEM | 512 KiB | 128 KiB |
| SQE | `a660_sqe.fw` | `a630_sqe.fw` |
| zap | `a660_zap.mdt` + `.b00`/`.b01`/`.b02` | `a610_zap.mdt` + `.b00`/`.b01`/`.b02` |
| recover | `gpu_recover=1` resets GX | leave `gpu_recover=0` (A610 GX reset hang) |

Firmware is the vendor's and never goes into git. Redwood pulls `a660_{gmu.bin,sqe.fw,zap.*}` from the phone's
`/vendor/firmware`. Citrus pulls `a610_zap.*` from the same place; `a630_sqe.fw` is not on the POCO M3 vendor
image — take it from [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) `qcom/a630_sqe.fw` into
`fw-gpu/`. `userland/build.sh` warns if `a610_zap.mdt` is there without `a630_sqe.fw`, or if neither
`a660_gmu.bin` nor `a610_zap.mdt` is present.

Citrus still boots with `cpus=3`. Do not default `gpu=1` until bring-up is proven. Validation cmdline:
`bootcpu=1 cpus=3 citrus_touch=1 gpu=1 gpu_recover=0`, with `GL=1` images.

## 4. Firmware

Vendor firmware never goes into the repository; it comes from the phone's own Android (`FIRMWARE` in the profile
lists what to pull, see README "On the phone"). Without `fw-gpu/` the kernel does not start the GPU and says so only
deep in the log (`adreno: THE GPU IS NOT STARTED`); `userland/build.sh` prints which firmware went into the image
and warns if the GPU set is incomplete (a660 GMU on redwood, a610 zap + `a630_sqe.fw` on citrus).

## 5. Run

```sh
common/build.sh <codename>
common/run.sh <codename>
```

The first useful milestone is the loader's own screen and log over USB (`./xl log`), the next one is the kernel's
verbose boot text (`-v`), then SpringBoard. Read [checks.md](checks.md) before trusting a "it is not in the log".
