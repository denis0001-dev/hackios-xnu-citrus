# HackiOS MCP server

Stdio MCP server for driving a phone (or reading logs) the same way we do in interactive sessions: `./xl`, `fastboot`, and files under `logs/`.

**Cursor:** `.cursor/mcp.json` runs `python3 mcp/server.py` with `XL_DEVICE=citrus` by default.

## Tools (chat workflow)

| Tool | What it does |
|------|----------------|
| `get_phone_status` | `xl state`, `fastboot getvar` (result, product, version, screen), run lock |
| `inspect_phone` | Status + boot-progress lines from the live log + screenshot |
| `poll_boot_progress` | `result` + filtered log tail (no screenshot); safe while the log is quiet |
| `boot_into_loader` | `./xl enter` |
| `start_kernel` | Background `./xl run` with `XL_SECS`, `XL_CMDLINE`, `XL_RAMDISK`, `XL_FOLLOW` |
| `start_citrus_userspace` | Same as the POCO M3 session: `ufs=2 root_apfs=ufs_userdata bootcpu=1 cpus=1`, long timer, live follow |
| `start_log_follower` | Background `mcp/live_log.py` (2 s poll until run ends) |
| `stop_kernel` | `./xl stop` (`confirm=true`) |
| `reboot_to_android` | `./xl reboot` (`confirm=true`) |
| `reinstall_loader` | `./xl reload` (`confirm=true`) |
| `rebuild_loader` | `xnuloader/build.sh` + `./xl reload` (`confirm=true`) |
| `upload_system_image` | `./xl ramdisk` (resident APFS; `file=none` drops it) |
| `fetch_kernel_log` | `./xl log` during a run |
| `capture_phone_screen` | `./xl screenshot` |
| `get_fastboot_variable` | e.g. `result`, `product`, `screen` |
| `list_devices` / `show_device_config` | `common/devices/*.conf` (packing/boot layout) |
| `pack_boot_image` | `mkxnuimg.py` with the device profile (wrong `BOOT_HEADER` → stock fastboot) |
| `list_boot_logs` / `read_boot_log` | On-disk `logs/*` and `*-run.log.live` |
| `find_boot_progress` | Grep SpringBoard, BootUI, panics, MFB swaps, etc. |

Destructive calls need `"confirm": true`.

## Prompts

- `boot_desktop` — reach SpringBoard, verify screenshot is not the Apple BootUI logo.
- `diagnose_boot` — `result: running` means the kernel is up; silence after framebuffer swaps is normal.

## Manual live log

```bash
XL_DEVICE=citrus python3 mcp/live_log.py --out logs/$(date +%H%M%S)-run.log.live
```

## Notes

- `result: running` means the kernel is still on its core. Do not treat `lsd` CPU-monitor spam or a quiet minute after MiniIOMobileFramebuffer swaps as a hang.
- `./xl run` with `XL_SECS=240` only waits ~340 s on the host for `getvar result`; the kernel may still be running after that.
