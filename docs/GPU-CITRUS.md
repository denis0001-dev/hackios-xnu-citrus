# Adreno 610 GPU on POCO M3 (citrus)

Citrus uses the **same Adreno 610 + GMU-wrapper** model as [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (SM6225), not HackiOS redwood’s **642L full-GMU** path (`a660_gmu.bin`).

## Working reference ports

See the canvas report [Adreno GPU — working ios/XNU ports only](/home/denis0001-dev/.cursor/projects/home-denis0001-dev-hackios-hackios-xnu-kernel/canvases/adreno-working-gpu-ports.canvas.tsx). For citrus, copy **ios-topaz** (`xnu/patches/0901`–`0917` kernel GPU, `userland/gpu`, `userland/mesa`), not experimental cold-PAS / cmd-db / Android-warm tooling.

## This tree

| Piece | Role |
|--------|------|
| `xnu/files/iokit/Kernel/arm/TopazAdreno.cpp` | Vendored from ios-topaz patch **0901** (TopazGpuW / `redwood_gpu_*` for Mesa) |
| `xnu/files/bsd/dev/topaz_fw.c` | Root-fs firmware read + `hackios_gpu_is_adreno610()` (`a610_zap` in `fw/`, no `a660_gmu`) |
| `xnu/files/bsd/dev/redwood_adreno.c` | **642L only** when `HACKIOS_ADRENO_610` is unset; `gpu=1` auto-start skipped if `a610_zap` is present |
| `iokit/Kernel/RedwoodGPU.cpp` | Shared msm DRM client (unchanged) |
| `MiniIOMobileFramebuffer.cpp` | Calls `topaz_adreno_init()` on display start (`gpu=1`) |

Build citrus GPU kernel:

```bash
DEVICE=citrus ./xnu-build.sh -j"$(nproc)"
# or: common/build.sh citrus GL=1
```

Firmware in the boot image (`FIRMWARE=fw-gpu` in `citrus.conf`): `a610_zap.mdt`, `a610_zap.b02`, `a630_sqe.fw`. Userland should install them under `/usr/share/firmware/citrus/` (same names as topaz, citrus path).

## Patch parity (topaz → citrus)

| ios-topaz patch | Status in this repo |
|-----------------|---------------------|
| 0901 TopazGpuW + RedwoodGPU | **Imported** (TopazAdreno + existing RedwoodGPU) |
| 0904 ordinary-page GPU buffers + cache maint (0906-style) | **Imported** in TopazAdreno + RedwoodGPU default `gpu_cached` |
| 0903 36-bit SMMU / upper DDR | **Partial** — citrus payload RAM is &lt;4 GiB (`0x80000000`); full 0903 mask if RAM moves high |
| 0905–0917 embedded FW, pool, identity | **TODO** |
| 0131–0139 SweetSurface / inject | HackiOS uses **MiniIOSurface** — port as needed |
| userland/gpu + mesa | **hackios-xnu-userland** with `GL=1` |

## Boot args

- `gpu=1` — TopazAdreno thread; firmware from **`fw/`** in the boot image (cold) or `/usr/share/firmware/citrus/` after mount; publishes RedwoodGPU for Mesa.
- `gpu_mhz=600` — optional core clock (topaz default recipe).
- `adreno=N` — staged 610 bring-up without Mesa: starts the Topaz thread even when `gpu=0`. Gates: **1** map, **11** power/clocks, **12** TrustZone zap PAS, **13** SMMU, **15** CP/SQE (“THE 3D CORE TAKES COMMANDS”). RedwoodGPU runs only when **`gpu=1`** and step **15** completed.
- Success strings in the log: `TRUSTZONE ACCEPTS`, `AUTHENTICATED`, `adreno: step 15 done`, `THE 3D CORE TAKES COMMANDS`.
- Do **not** use redwood’s 22-step **642L GMU** ladder on citrus.

## SM6115 vs SM6225

Same 610 block (`0x5900000`, wrapper `0x596a000`). Diff PMIC (PM6125), Novatek panel, WCN3980 Wi‑Fi — not the GPU sequencing in [topaz-woa P8_gpu.md](https://github.com/MakrSas/topaz-woa/blob/master/docs/P8_gpu.md).
