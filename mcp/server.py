#!/usr/bin/env python3
"""HackiOS MCP server (stdio, JSON-RPC with Content-Length framing).

Zero third-party deps. Cursor config: .cursor/mcp.json
Workspace root is the parent of this file's directory.
"""
from __future__ import annotations

import base64
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parent.parent
XL = ROOT / "xl"
LOGS = ROOT / "logs"
DEVICES = ROOT / "common" / "devices"
PHONE = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / "hackios-phone"
PROTOCOL = "2024-11-05"
SERVER_NAME = "hackios"
SERVER_VERSION = "1.1.0"

# --- stdio framing -----------------------------------------------------------

def _read_message() -> dict[str, Any] | None:
    headers: dict[str, str] = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode("utf-8", "replace").rstrip("\r\n")
        if line == "":
            break
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    n = int(headers.get("content-length", "0") or 0)
    if n <= 0:
        return None
    body = sys.stdin.buffer.read(n)
    if not body:
        return None
    return json.loads(body.decode("utf-8"))


def _write_message(msg: dict[str, Any]) -> None:
    raw = json.dumps(msg, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii") + raw)
    sys.stdout.buffer.flush()


def _ok(id_: Any, result: Any) -> None:
    _write_message({"jsonrpc": "2.0", "id": id_, "result": result})


def _err(id_: Any, code: int, message: str) -> None:
    _write_message({"jsonrpc": "2.0", "id": id_, "error": {"code": code, "message": message}})


def text(s: str) -> dict[str, Any]:
    return {"type": "text", "text": s}


def image_png(path: Path) -> list[dict[str, Any]]:
    data = path.read_bytes()
    out: list[dict[str, Any]] = [
        text(f"{path} ({len(data)} bytes)"),
        {"type": "image", "mimeType": "image/png", "data": base64.b64encode(data).decode("ascii")},
    ]
    return out


def tool_result(content: str | list[dict[str, Any]], is_error: bool = False) -> dict[str, Any]:
    if isinstance(content, str):
        content = [text(content)]
    return {"content": content, "isError": is_error}


# --- helpers -----------------------------------------------------------------

def default_device() -> str:
    env = os.environ.get("XL_DEVICE", "").strip()
    if env:
        return env
    names = device_names()
    if "citrus" in names:
        return "citrus"
    if "redwood" in names:
        return "redwood"
    return names[0] if names else "redwood"


def device_names() -> list[str]:
    return sorted(p.stem for p in DEVICES.glob("*.conf") if not p.name.startswith("_"))


def parse_conf(name: str) -> dict[str, str]:
    path = DEVICES / f"{name}.conf"
    if not path.is_file():
        raise FileNotFoundError(f"no profile {path}")
    out: dict[str, str] = {"DEVICE": name}
    for line in path.read_text(errors="replace").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if "=" not in s:
            continue
        k, v = s.split("=", 1)
        k = k.strip()
        v = v.split("#", 1)[0].strip().strip('"')
        out[k] = v
    return out


def run_cmd(
    argv: list[str],
    *,
    env: dict[str, str] | None = None,
    timeout: float | None = 60,
    cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    e = os.environ.copy()
    if env:
        e.update(env)
    e.setdefault("XL_DEVICE", default_device())
    return subprocess.run(
        argv,
        cwd=str(cwd or ROOT),
        env=e,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )


def xl(*args: str, env: dict[str, str] | None = None, timeout: float | None = 90) -> str:
    p = run_cmd([str(XL), *args], env=env, timeout=timeout)
    out = (p.stdout or "").rstrip()
    if p.returncode != 0 and not out:
        out = f"xl {' '.join(args)} exited {p.returncode}"
    return out


def fastboot_getvar(name: str, timeout: float = 8) -> str:
    try:
        p = subprocess.run(
            ["fastboot", "getvar", name],
            cwd=str(ROOT),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        return f"(fastboot {name}: {e})"
    blob = p.stdout or ""
    for line in blob.splitlines():
        if line.lower().startswith(name.lower() + ":"):
            return line.strip()
    return blob.strip() or f"{name}: (empty)"


def spawn(
    argv: list[str],
    log_path: Path,
    env: dict[str, str] | None = None,
) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    e = os.environ.copy()
    if env:
        e.update(env)
    logf = open(log_path, "ab")
    p = subprocess.Popen(
        argv,
        cwd=str(ROOT),
        env=e,
        stdout=logf,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    return p.pid


def pick(args: dict[str, Any], *keys: str, default: Any = None) -> Any:
    for k in keys:
        if k in args and args[k] is not None:
            return args[k]
    return default


def device_from(args: dict[str, Any]) -> str:
    return str(pick(args, "XL_DEVICE", "device", default=default_device()))


def need_confirm(args: dict[str, Any]) -> str | None:
    if args.get("confirm") is True:
        return None
    return "refused: pass confirm=true for this destructive action"


def clip(s: str, limit: int = 80_000) -> str:
    if len(s) <= limit:
        return s
    return f"... [{len(s) - limit} bytes omitted from the start] ...\n" + s[-limit:]


NOISE_RE = re.compile(
    r"(denied attempt to escape CPU monitor|Last log repeated \d+ times)",
    re.I,
)


def filter_log(s: str, *, grep: str | None, tail: int | None, noise: bool) -> str:
    lines = s.splitlines()
    if not noise:
        lines = [l for l in lines if not NOISE_RE.search(l)]
    if grep:
        try:
            rx = re.compile(grep)
            lines = [l for l in lines if rx.search(l)]
        except re.error as e:
            return f"bad grep: {e}"
    if tail and tail > 0:
        lines = lines[-tail:]
    return "\n".join(lines)


# --- tools -------------------------------------------------------------------

def t_phone_state(_args: dict[str, Any]) -> dict[str, Any]:
    st = xl("state", timeout=20)
    lines = [
        f"device={default_device()}",
        f"xl state: {st or '(empty)'}",
        fastboot_getvar("product"),
        fastboot_getvar("version"),
        fastboot_getvar("result"),
        fastboot_getvar("screen"),
    ]
    who = PHONE.with_suffix(".who")
    ram = PHONE.with_suffix(".ramdisk")
    running = False
    try:
        # lock held => a run() is in this copy's xl process
        import fcntl

        fd = os.open(str(PHONE) + ".run", os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fcntl.flock(fd, fcntl.LOCK_UN)
        except BlockingIOError:
            running = True
        finally:
            os.close(fd)
    except OSError:
        pass
    lines.append(f"xl run lock held: {running}")
    if who.is_file():
        lines.append(f"who: {who.read_text().strip()}")
    if ram.is_file():
        lines.append(f"resident ramdisk record: {ram.read_text().strip()}")
    lines.append(
        "result: running means the kernel is still on its core. "
        "Do not treat lsd CPU-monitor lines or a quiet minute after them as a hang."
    )
    return tool_result("\n".join(lines))


def t_poll_boot_progress(args: dict[str, Any]) -> dict[str, Any]:
    """Lightweight status while a run is live — no screenshot, no stop."""
    device = device_from(args)
    lines = [
        fastboot_getvar("result"),
        fastboot_getvar("product"),
        fastboot_getvar("screen"),
    ]
    tail = int(pick(args, "tail", default=60) or 60)
    noise = bool(args.get("noise", True))
    try:
        t_xl_log({"device": device, "tail": tail, "noise": noise})
        path = LOGS / "loader.log"
        if path.is_file():
            body = filter_log(path.read_text(errors="replace"), grep=args.get("grep"), tail=tail, noise=noise)
            lines.append(f"--- log tail ({path.name}, {tail} lines) ---")
            lines.append(clip(body, 40_000))
    except subprocess.TimeoutExpired:
        lines.append("xl log timed out (USB busy?)")
    lines.append(
        "If result is running, the kernel is up. A quiet minute after MiniIOMobileFramebuffer "
        "swap lines is normal; lsd CPU-monitor denials are noise."
    )
    return tool_result("\n".join(lines))


def t_start_citrus_userspace(args: dict[str, Any]) -> dict[str, Any]:
    """POCO M3 / citrus SpringBoard recipe used in bring-up sessions."""
    device = str(pick(args, "device", default="citrus"))
    secs = int(pick(args, "seconds", "XL_SECS", default=240) or 240)
    follow = int(pick(args, "follow_seconds", "XL_FOLLOW", default=10) or 10)
    cmdline = str(
        pick(
            args,
            "boot_args",
            "XL_CMDLINE",
            default="ufs=2 root_apfs=ufs_userdata bootcpu=1 cpus=1",
        )
    )
    ramdisk = str(pick(args, "ramdisk", "XL_RAMDISK", default="userland/root.apfs"))
    return t_xl_run(
        {
            "device": device,
            "seconds": secs,
            "boot_args": cmdline,
            "ramdisk": ramdisk,
            "follow_seconds": follow,
            "springboard_delay": pick(args, "springboard_delay", "XL_SB_DELAY", default=8),
        }
    )


def t_start_log_follower(args: dict[str, Any]) -> dict[str, Any]:
    stamp = time.strftime("%H%M%S")
    out = Path(str(pick(args, "out", default=f"logs/{stamp}-run.log.live")))
    if not out.is_absolute():
        out = ROOT / out
    hostlog = LOGS / f"{stamp}-live-follow.host.log"
    env = os.environ.copy()
    env.setdefault("XL_DEVICE", device_from(args))
    pid = spawn(
        [sys.executable, str(ROOT / "mcp" / "live_log.py"), "--out", str(out)],
        hostlog,
        env=env,
    )
    return tool_result(
        f"started live log follower pid={pid}\n"
        f"writes: {out}\nhost stdout: {hostlog}\n"
        "Stops when fastboot result is no longer running."
    )


def t_xl_enter(args: dict[str, Any]) -> dict[str, Any]:
    env = {"XL_DEVICE": device_from(args)}
    out = xl("enter", env=env, timeout=240)
    return tool_result(out or "loader is up")


def t_xl_run(args: dict[str, Any]) -> dict[str, Any]:
    device = device_from(args)
    # Same defaults as ./xl: XL_SECS=8, XL_CMDLINE=-v, XL_RAMDISK=userland/root.apfs, no XL_FOLLOW.
    secs = pick(args, "seconds", "XL_SECS", "secs", default=8)
    cmdline = pick(args, "boot_args", "XL_CMDLINE", "cmdline", default="-v")
    ramdisk = pick(args, "ramdisk", "XL_RAMDISK", default="userland/root.apfs")
    follow = pick(args, "follow_seconds", "XL_FOLLOW", "follow", default=None)
    sb_delay = pick(args, "springboard_delay", "XL_SB_DELAY", "sb_delay", default=None)
    checkpoint = pick(args, "checkpoint", "N", default=None)
    env = {
        "XL_DEVICE": device,
        "XL_SECS": str(secs),
        "XL_CMDLINE": str(cmdline),
        "XL_RAMDISK": str(ramdisk),
    }
    if follow is not None:
        env["XL_FOLLOW"] = str(follow)
    if sb_delay is not None:
        env["XL_SB_DELAY"] = str(sb_delay)
    if args.get("XL_FORCE") or args.get("force"):
        env["XL_FORCE"] = "1"
    argv = [str(XL), "run"]
    if checkpoint is not None:
        argv.append(str(checkpoint))
    stamp = time.strftime("%H%M%S")
    hostlog = LOGS / f"{stamp}-mcp-run.host.log"
    pid = spawn(argv, hostlog, env=env)
    shown = " ".join(
        f"{k}={env[k]}" for k in ("XL_DEVICE", "XL_SECS", "XL_CMDLINE", "XL_RAMDISK", "XL_FOLLOW", "XL_SB_DELAY") if k in env
    )
    extra = f" ./xl run {checkpoint}" if checkpoint is not None else " ./xl run"
    return tool_result(f"started `{shown}{extra}` pid={pid}\nhost stdout: {hostlog}")


def t_xl_stop(args: dict[str, Any]) -> dict[str, Any]:
    if (msg := need_confirm(args)):
        return tool_result(msg, is_error=True)
    env = {"XL_DEVICE": device_from(args)}
    if args.get("XL_FORCE") or args.get("force"):
        env["XL_FORCE"] = "1"
    return tool_result(xl("stop", env=env, timeout=30))


def t_xl_reboot(args: dict[str, Any]) -> dict[str, Any]:
    if (msg := need_confirm(args)):
        return tool_result(msg, is_error=True)
    env = {"XL_DEVICE": device_from(args)}
    if args.get("XL_FORCE") or args.get("force"):
        env["XL_FORCE"] = "1"
    return tool_result(xl("reboot", env=env, timeout=30) or "reboot sent")


def t_xl_reload(args: dict[str, Any]) -> dict[str, Any]:
    if (msg := need_confirm(args)):
        return tool_result(msg, is_error=True)
    env = {"XL_DEVICE": device_from(args)}
    return tool_result(xl("reload", env=env, timeout=300))


def t_xl_ramdisk(args: dict[str, Any]) -> dict[str, Any]:
    path = str(pick(args, "file", "FILE", "path", default="userland/system.apfs"))
    if path == "none":
        if (msg := need_confirm(args)):
            return tool_result(msg, is_error=True)
    env = {"XL_DEVICE": device_from(args)}
    out = xl("ramdisk", path, env=env, timeout=float(args.get("timeout") or 1800))
    return tool_result(out)


def t_xl_log(args: dict[str, Any]) -> dict[str, Any]:
    env = {"XL_DEVICE": device_from(args)}
    try:
        raw = xl("log", env=env, timeout=40)
    except subprocess.TimeoutExpired:
        return tool_result("xl log timed out (USB lock?). Retry.", is_error=True)
    path = LOGS / "loader.log"
    body = path.read_text(errors="replace") if path.is_file() else (raw or "")
    if not body:
        return tool_result("xl log: empty", is_error=True)
    shown = filter_log(
        body,
        grep=pick(args, "grep"),
        tail=int(args["tail"]) if args.get("tail") is not None else None,
        noise=bool(args.get("noise", True)),
    )
    return tool_result(f"./xl log -> {path} {len(body)} bytes\n" + clip(shown))


def t_xl_screenshot(args: dict[str, Any]) -> dict[str, Any]:
    env = {"XL_DEVICE": device_from(args)}
    dest = str(pick(args, "path", "F", default="logs/screen.png"))
    out = xl("screenshot", dest, env=env, timeout=60)
    p = Path(dest)
    if not p.is_absolute():
        p = ROOT / dest
    if p.is_file() and p.stat().st_size > 32:
        return tool_result([text(out or f"./xl screenshot {dest}"), *image_png(p)[1:]])
    return tool_result(out or "screenshot failed", is_error=True)


def t_fastboot_getvar(args: dict[str, Any]) -> dict[str, Any]:
    name = str(pick(args, "name", default="result"))
    return tool_result(fastboot_getvar(name))


def t_list_devices(_args: dict[str, Any]) -> dict[str, Any]:
    rows = []
    for n in device_names():
        c = parse_conf(n)
        rows.append(f"{n}: {c.get('NAME','?')} kind={c.get('KIND','?')} soc={c.get('SOC','?')} bootargs={c.get('BOOTARGS','')}")
    return tool_result("\n".join(rows) or "no device profiles")


def t_device_info(args: dict[str, Any]) -> dict[str, Any]:
    name = device_from(args)
    c = parse_conf(name)
    body = "\n".join(f"{k}={v}" for k, v in c.items())
    plat = DEVICES / f"{name}.platform.c"
    extra = f"\n\nplatform source: {plat}" if plat.is_file() else ""
    return tool_result(body + extra)


def t_list_logs(args: dict[str, Any]) -> dict[str, Any]:
    LOGS.mkdir(exist_ok=True)
    glob = args.get("glob") or "*"
    files = sorted(LOGS.glob(glob), key=lambda p: p.stat().st_mtime, reverse=True)
    limit = int(args.get("limit") or 40)
    rows = []
    for p in files[:limit]:
        st = p.stat()
        rows.append(f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(st.st_mtime))}  {st.st_size:9d}  {p.name}")
    return tool_result("\n".join(rows) or "logs/ is empty")


def t_read_log(args: dict[str, Any]) -> dict[str, Any]:
    name = args.get("path") or "logs/loader.log"
    p = Path(name)
    if not p.is_absolute():
        p = ROOT / p
    if not p.is_file():
        # also try logs/
        q = LOGS / Path(name).name
        if q.is_file():
            p = q
        else:
            return tool_result(f"not found: {name}", is_error=True)
    body = p.read_text(errors="replace")
    filtered = filter_log(
        body,
        grep=args.get("grep"),
        tail=int(args["tail"]) if args.get("tail") is not None else 200,
        noise=bool(args.get("noise")),
    )
    return tool_result(f"{p} {p.stat().st_size} bytes\n" + clip(filtered))


BOOT_MARKERS = (
    "panic",
    "Synchronous Exception",
    "SpringBoard",
    "backboardd",
    "BootUI",
    "Apple Logo",
    "redwood: unlock",
    "battery: SOC",
    "MiniIOMobileFramebuffer: swap",
    "loader_return",
    "result:",
    "the kernel hung",
    "successfully spawned",
)


def t_boot_markers(args: dict[str, Any]) -> dict[str, Any]:
    name = args.get("path") or "logs/loader.log"
    p = Path(name)
    if not p.is_absolute():
        p = ROOT / p
    if not p.is_file():
        q = LOGS / Path(name).name
        p = q if q.is_file() else p
    if not p.is_file():
        return tool_result(f"not found: {name}", is_error=True)
    body = p.read_text(errors="replace")
    hits: list[str] = []
    extra = args.get("also") or ""
    keys = list(BOOT_MARKERS) + [s for s in extra.split(",") if s.strip()]
    for line in body.splitlines():
        if any(k.lower() in line.lower() for k in keys):
            if NOISE_RE.search(line) and not args.get("noise"):
                continue
            hits.append(line)
    tail = int(args.get("tail") or 80)
    shown = hits[-tail:]
    return tool_result(
        f"{p}: {len(hits)} marker lines, showing last {len(shown)}\n" + "\n".join(shown)
    )


def t_mkxnuimg(args: dict[str, Any]) -> dict[str, Any]:
    device = device_from(args)
    c = parse_conf(device)
    env = {
        "XL_DEVICE": device,
        "FD_BASE": c.get("FD_BASE", ""),
        "FD_SIZE": c.get("FD_SIZE", ""),
        "PAYLOAD_BASE": c.get("PAYLOAD_BASE", ""),
        "BOOT_HEADER": c.get("BOOT_HEADER", ""),
        "DTB": c.get("DTB", ""),
        "UEFI_TARGET": c.get("UEFI_TARGET", ""),
    }
    payload = args.get("payload") or str(ROOT / "out" / "usb-payload.bin")
    out = args.get("output") or str(ROOT / "out" / f"{device}-boot.img")
    p = run_cmd(
        ["python3", str(ROOT / "mkxnuimg.py"), "-p", payload, "-o", out],
        env=env,
        timeout=60,
    )
    size = Path(out).stat().st_size if Path(out).is_file() else 0
    return tool_result(
        f"{p.stdout or ''}\n{out} {size} bytes BOOT_HEADER={env['BOOT_HEADER']} "
        f"PAYLOAD_BASE={env['PAYLOAD_BASE']}\nexit {p.returncode}",
        is_error=p.returncode != 0,
    )


def _content(result: dict[str, Any]) -> list[dict[str, Any]]:
    return list(result.get("content") or [])


def t_look_at_phone(args: dict[str, Any]) -> dict[str, Any]:
    """Status + live kernel log markers + display, one round trip."""
    chunks: list[dict[str, Any]] = [text("== where the phone is ==")]
    try:
        chunks.extend(_content(t_phone_state(args)))
    except Exception as e:
        chunks.append(text(f"status failed: {e}"))
    chunks.append(text("== kernel log (tail) =="))
    try:
        log_res = t_xl_log({
            **args,
            "noise": bool(args.get("noise", True)),
            "tail": int(pick(args, "tail", default=120) or 120),
        })
        chunks.extend(_content(log_res))
        chunks.append(text("== boot markers =="))
        chunks.extend(_content(t_boot_markers({
            "path": "logs/loader.log",
            "tail": int(pick(args, "marker_tail", default=40) or 40),
            "noise": bool(args.get("noise", False)),
        })))
    except Exception as e:
        chunks.append(text(f"log failed: {e}"))
    chunks.append(text("== display =="))
    try:
        shot = t_xl_screenshot({**args, "F": pick(args, "path", "F", default="logs/screen.png")})
        chunks.extend(_content(shot))
        if shot.get("isError"):
            return tool_result(chunks, is_error=True)
    except Exception as e:
        chunks.append(text(f"screenshot failed: {e}"))
        return tool_result(chunks, is_error=True)
    return tool_result(chunks)


def t_rebuild_and_reload(args: dict[str, Any]) -> dict[str, Any]:
    """Build xnuloader for the device, then reboot the phone into that loader."""
    if (msg := need_confirm(args)):
        return tool_result(msg, is_error=True)
    device = device_from(args)
    p = run_cmd(["bash", str(ROOT / "xnuloader" / "build.sh"), device], timeout=300)
    build_out = (p.stdout or "").rstrip()
    if p.returncode != 0:
        return tool_result(f"loader build failed\n{build_out}", is_error=True)
    env = {"XL_DEVICE": device}
    reload_out = xl("reload", env=env, timeout=300)
    return tool_result(f"loader built for {device}\n{build_out}\n\nreinstall:\n{reload_out}")


def schema(props: dict[str, Any], required: list[str] | None = None) -> dict[str, Any]:
    return {"type": "object", "properties": props, "required": required or [], "additionalProperties": False}


DEVICE = {
    "type": "string",
    "description": "Target board: citrus (POCO M3), redwood (POCO X5 Pro), qemu-virt. Default: env XL_DEVICE.",
}
CONFIRM = {
    "type": "boolean",
    "description": "Must be true: this changes the phone or QEMU.",
}

TOOLS: list[dict[str, Any]] = [
    {
        "name": "get_phone_status",
        "title": "Get phone status",
        "description": "Where the phone is right now: android, stock fastboot, XNU loader, or absent. Also returns live USB result (running/idle), product, loader version, screen size, and whether a kernel run lock is held.",
        "inputSchema": schema({"device": DEVICE}),
        "handler": t_phone_state,
    },
    {
        "name": "inspect_phone",
        "title": "Inspect phone (status, log, screen)",
        "description": "One round trip: phone status, boot-progress lines from the live kernel log, and a screenshot of the display. Use this while a kernel run is in progress.",
        "inputSchema": schema({
            "device": DEVICE,
            "path": {"type": "string", "description": "Screenshot PNG path. Default logs/screen.png."},
            "tail": {"type": "integer", "description": "How many log lines to consider. Default 80."},
        }),
        "handler": t_look_at_phone,
    },
    {
        "name": "boot_into_loader",
        "title": "Boot into the XNU loader",
        "description": "Leave Android (or stock fastboot) and start the in-RAM XNU loader over USB. Does nothing if the loader is already answering.",
        "inputSchema": schema({"device": DEVICE}),
        "handler": t_xl_enter,
    },
    {
        "name": "start_kernel",
        "title": "Start the kernel",
        "description": "Upload the current kernel and boot it on the phone. Returns immediately; the run continues in the background until the kernel comes back or its timer expires. Defaults: 8 second timer, boot-args -v, payload ramdisk userland/root.apfs.",
        "inputSchema": schema({
            "device": DEVICE,
            "boot_args": {"type": "string", "description": "Kernel boot-args (XL_CMDLINE). Default -v."},
            "seconds": {"type": "integer", "description": "How long until the kernel returns by itself. Default 8."},
            "ramdisk": {"type": "string", "description": "Small image sent with the kernel. Default userland/root.apfs. none = send none."},
            "follow_seconds": {"type": "integer", "description": "If set, save a live kernel log every N seconds."},
            "springboard_delay": {"type": "integer", "description": "Seconds SpringBoard waits before starting. Default 8."},
            "checkpoint": {"type": "integer", "description": "Stop at kernel checkpoint N instead of a full boot."},
        }),
        "handler": t_xl_run,
    },
    {
        "name": "poll_boot_progress",
        "title": "Poll boot progress",
        "description": "fastboot result + filtered kernel log tail. Use while a run is live and the log goes quiet; does not capture the screen or stop the kernel.",
        "inputSchema": schema({
            "device": DEVICE,
            "tail": {"type": "integer", "description": "Log lines. Default 60."},
            "grep": {"type": "string"},
            "noise": {"type": "boolean", "description": "Default true. false hides lsd CPU-monitor spam."},
        }),
        "handler": t_poll_boot_progress,
    },
    {
        "name": "start_citrus_userspace",
        "title": "Start citrus SpringBoard boot",
        "description": "Background ./xl run with ufs=2 root_apfs=ufs_userdata bootcpu=1 cpus=1, userland/root.apfs, XL_SECS=240, XL_FOLLOW=10 (the POCO M3 bring-up recipe). Resident flashboot/system APFS must already be in RAM unless you uploaded it earlier.",
        "inputSchema": schema({
            "device": {"type": "string", "description": "Default citrus."},
            "seconds": {"type": "integer", "description": "loader_return_after. Default 240."},
            "follow_seconds": {"type": "integer", "description": "Live log snapshot interval. Default 10."},
            "boot_args": {"type": "string"},
            "ramdisk": {"type": "string"},
            "springboard_delay": {"type": "integer", "description": "sb_delay. Default 8."},
        }),
        "handler": t_start_citrus_userspace,
    },
    {
        "name": "start_log_follower",
        "title": "Start live log follower",
        "description": "Background mcp/live_log.py: ./xl log every 2 s until result is no longer running. Writes logs/HHMMSS-run.log.live.",
        "inputSchema": schema({
            "device": DEVICE,
            "out": {"type": "string", "description": "Output path. Default logs/<stamp>-run.log.live."},
        }),
        "handler": t_start_log_follower,
    },
    {
        "name": "stop_kernel",
        "title": "Stop the kernel",
        "description": "Ask the running kernel to return to the loader, as if its timer expired. Requires confirm=true. Do not use this just because the log went quiet.",
        "inputSchema": schema({"device": DEVICE, "confirm": CONFIRM, "force": {"type": "boolean"}}, ["confirm"]),
        "handler": t_xl_stop,
    },
    {
        "name": "reboot_to_android",
        "title": "Reboot to Android",
        "description": "Cold-reset the phone back to Android. Drops the in-RAM system image. Requires confirm=true.",
        "inputSchema": schema({"device": DEVICE, "confirm": CONFIRM, "force": {"type": "boolean"}}, ["confirm"]),
        "handler": t_xl_reboot,
    },
    {
        "name": "reinstall_loader",
        "title": "Reinstall the loader",
        "description": "Reboot the phone and boot a loader image that is already built. Requires confirm=true.",
        "inputSchema": schema({"device": DEVICE, "confirm": CONFIRM}, ["confirm"]),
        "handler": t_xl_reload,
    },
    {
        "name": "rebuild_loader",
        "title": "Rebuild and reinstall the loader",
        "description": "Compile xnuloader for this device, then reboot the phone into that new loader. Requires confirm=true.",
        "inputSchema": schema({"device": DEVICE, "confirm": CONFIRM}, ["confirm"]),
        "handler": t_rebuild_and_reload,
    },
    {
        "name": "upload_system_image",
        "title": "Upload system image into RAM",
        "description": "Copy an APFS image into phone RAM as the resident root filesystem. It stays across kernel runs until the phone reboots. Default file: userland/system.apfs. file=none drops it (confirm=true). Refused while a kernel run is in progress.",
        "inputSchema": schema({
            "device": DEVICE,
            "file": {"type": "string", "description": "Image path, or none to drop. Default userland/system.apfs."},
            "timeout": {"type": "integer"},
            "confirm": CONFIRM,
        }),
        "handler": t_xl_ramdisk,
    },
    {
        "name": "fetch_kernel_log",
        "title": "Fetch the kernel log",
        "description": "Pull the loader log, and the kernel log so far if a run is live, and return the text. Works during a run.",
        "inputSchema": schema({
            "device": DEVICE,
            "grep": {"type": "string", "description": "Optional regex filter on the returned text."},
            "tail": {"type": "integer", "description": "Optional last N lines only."},
            "noise": {"type": "boolean", "description": "Default true = full log. false drops CPU-monitor spam."},
        }),
        "handler": t_xl_log,
    },
    {
        "name": "capture_phone_screen",
        "title": "Capture the phone screen",
        "description": "Save the phone framebuffer as a PNG and return the image. Works during a kernel run. Apple logo means BootUI is still up; desktop means lock or home UI.",
        "inputSchema": schema({
            "device": DEVICE,
            "path": {"type": "string", "description": "Output PNG. Default logs/screen.png."},
        }),
        "handler": t_xl_screenshot,
    },
    {
        "name": "get_fastboot_variable",
        "title": "Read a fastboot variable",
        "description": "Read one USB fastboot variable from the phone. Useful names: result (running/idle), product, version, screen.",
        "inputSchema": schema({"name": {"type": "string", "description": "Variable name. Default result."}}),
        "handler": t_fastboot_getvar,
    },
    {
        "name": "list_devices",
        "title": "List device profiles",
        "description": "List boards this tree can build and boot: citrus, redwood, qemu-virt, with SoC and default boot-args.",
        "inputSchema": schema({}),
        "handler": t_list_devices,
    },
    {
        "name": "show_device_config",
        "title": "Show device config",
        "description": "Show one board's profile: memory layout, DTB, boot-args, firmware bits.",
        "inputSchema": schema({"device": DEVICE}),
        "handler": t_device_info,
    },
    {
        "name": "list_boot_logs",
        "title": "List boot logs",
        "description": "List files in logs/ newest first: kernel runs, live follows, screenshots.",
        "inputSchema": schema({
            "glob": {"type": "string", "description": "Glob under logs/. Default *."},
            "limit": {"type": "integer"},
        }),
        "handler": t_list_logs,
    },
    {
        "name": "read_boot_log",
        "title": "Read a boot log",
        "description": "Read a log file already on disk. Does not talk to the phone.",
        "inputSchema": schema({
            "path": {"type": "string", "description": "Default logs/loader.log."},
            "grep": {"type": "string"},
            "tail": {"type": "integer"},
            "noise": {"type": "boolean", "description": "Default false drops CPU-monitor spam."},
        }),
        "handler": t_read_log,
    },
    {
        "name": "find_boot_progress",
        "title": "Find boot progress in a log",
        "description": "From a log, return lines about panics, SpringBoard, BootUI, framebuffer swaps, unlock, battery, and kernel return.",
        "inputSchema": schema({
            "path": {"type": "string", "description": "Default logs/loader.log."},
            "also": {"type": "string", "description": "Extra comma-separated substrings."},
            "tail": {"type": "integer"},
            "noise": {"type": "boolean"},
        }),
        "handler": t_boot_markers,
    },
    {
        "name": "pack_boot_image",
        "title": "Pack a boot image",
        "description": "Pack a fastboot boot image using this device's memory map and header version. Wrong board here puts a phone into stock fastboot.",
        "inputSchema": schema({
            "device": DEVICE,
            "payload": {"type": "string", "description": "Input payload.bin. Default out/usb-payload.bin."},
            "output": {"type": "string", "description": "Output boot.img. Default out/<device>-boot.img."},
        }),
        "handler": t_mkxnuimg,
    },
]

HANDLERS: dict[str, Callable[[dict[str, Any]], dict[str, Any]]] = {
    t["name"]: t["handler"] for t in TOOLS  # type: ignore[misc]
}


def tools_public() -> list[dict[str, Any]]:
    out = []
    for t in TOOLS:
        item = {"name": t["name"], "description": t["description"], "inputSchema": t["inputSchema"]}
        if t.get("title"):
            item["title"] = t["title"]
        out.append(item)
    return out


# --- resources / prompts -----------------------------------------------------

def resources_list() -> list[dict[str, Any]]:
    items = [
        {"uri": "hackios://devices", "name": "device profiles", "mimeType": "text/plain"},
        {"uri": "hackios://state", "name": "phone state", "mimeType": "text/plain"},
        {"uri": "hackios://log/loader", "name": "logs/loader.log", "mimeType": "text/plain"},
        {"uri": "hackios://screenshot", "name": "logs/desktop.png", "mimeType": "image/png"},
    ]
    live = sorted(LOGS.glob("*-run.log.live"), key=lambda p: p.stat().st_mtime, reverse=True)
    if live:
        items.append({"uri": f"hackios://log/live", "name": live[0].name, "mimeType": "text/plain"})
    return items


def resource_read(uri: str) -> dict[str, Any]:
    if uri == "hackios://devices":
        text_body = t_list_devices({})["content"][0]["text"]
        return {"contents": [{"uri": uri, "mimeType": "text/plain", "text": text_body}]}
    if uri == "hackios://state":
        text_body = t_phone_state({})["content"][0]["text"]
        return {"contents": [{"uri": uri, "mimeType": "text/plain", "text": text_body}]}
    if uri in ("hackios://log/loader", "hackios://log/live"):
        if uri.endswith("live"):
            lives = sorted(LOGS.glob("*-run.log.live"), key=lambda p: p.stat().st_mtime, reverse=True)
            path = lives[0] if lives else LOGS / "loader.log"
        else:
            path = LOGS / "loader.log"
        body = path.read_text(errors="replace") if path.is_file() else f"missing {path}"
        return {"contents": [{"uri": uri, "mimeType": "text/plain", "text": clip(body, 100_000)}]}
    if uri == "hackios://screenshot":
        path = LOGS / "desktop.png"
        if not path.is_file():
            path = LOGS / "screen.png"
        if not path.is_file():
            return {"contents": [{"uri": uri, "mimeType": "text/plain", "text": "no screenshot yet"}]}
        return {"contents": [{"uri": uri, "mimeType": "image/png", "blob": base64.b64encode(path.read_bytes()).decode("ascii")}]}
    raise FileNotFoundError(uri)


PROMPTS = [
    {
        "name": "boot_desktop",
        "description": "Boot HackiOS on a phone to SpringBoard and verify with a screenshot that is not the Apple BootUI logo.",
        "arguments": [{"name": "device", "description": "citrus or redwood", "required": False}],
    },
    {
        "name": "diagnose_boot",
        "description": "Read the live kernel log and screenshot without treating silence or lsd spam as a hang.",
        "arguments": [],
    },
]


def prompt_get(name: str, arguments: dict[str, Any] | None) -> dict[str, Any]:
    args = arguments or {}
    device = args.get("device") or default_device()
    if name == "boot_desktop":
        text_body = (
            f"Boot HackiOS on {device} to an interactive SpringBoard desktop.\n"
            "1. get_phone_status. If android, boot_into_loader. If stock fastboot (product not xnuloader), the boot image is wrong.\n"
            "2. Resident system image must already be in RAM; upload_system_image if this is a cold loader session.\n"
            "3. start_citrus_userspace (or start_kernel with the same boot_args and a long seconds).\n"
            "4. Follow with inspect_phone / fetch_kernel_log / find_boot_progress / capture_phone_screen.\n"
            "5. Do not stop_kernel or reboot_to_android while result is running just because the log is quiet.\n"
            "6. Done only when the screenshot is lock/home UI, not Silicium splash or the Apple logo overlay.\n"
        )
    elif name == "diagnose_boot":
        text_body = (
            "The kernel is up if get_fastboot_variable name=result is running. Use inspect_phone or poll_boot_progress. "
            "Filter lsd spam (noise=false). A quiet minute after MiniIOMobileFramebuffer swap lines is normal. "
            "Apple logo on capture_phone_screen is BootUI until SpringBoard owns the display. "
            "Host xl run may time out waiting for result while the kernel is still running — that is not a hang."
        )
    else:
        raise FileNotFoundError(name)
    return {
        "description": name,
        "messages": [{"role": "user", "content": {"type": "text", "text": text_body}}],
    }


# --- dispatch ----------------------------------------------------------------

def handle(msg: dict[str, Any]) -> None:
    mid = msg.get("id")
    method = msg.get("method")
    params = msg.get("params") or {}
    if method is None:
        return
    if mid is None:
        return  # notification
    try:
        if method == "initialize":
            _ok(mid, {
                "protocolVersion": PROTOCOL,
                "capabilities": {
                    "tools": {"listChanged": False},
                    "resources": {"listChanged": False, "subscribe": False},
                    "prompts": {"listChanged": False},
                },
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "instructions": (
                    "HackiOS phone tools from ./xl and fastboot. inspect_phone or poll_boot_progress during a boot; "
                    "start_citrus_userspace for the POCO M3 SpringBoard recipe. result:running means the kernel is up — "
                    "do not stop_kernel for log silence or lsd spam. stop_kernel, reboot_to_android, reinstall_loader, "
                    "rebuild_loader, and upload_system_image file=none need confirm=true."
                ),
            })
            return
        if method == "ping":
            _ok(mid, {})
            return
        if method == "tools/list":
            _ok(mid, {"tools": tools_public()})
            return
        if method == "tools/call":
            name = params.get("name")
            arguments = params.get("arguments") or {}
            fn = HANDLERS.get(name)
            if not fn:
                _ok(mid, tool_result(f"unknown tool {name}", is_error=True))
                return
            try:
                _ok(mid, fn(arguments if isinstance(arguments, dict) else {}))
            except subprocess.TimeoutExpired:
                _ok(mid, tool_result(f"{name} timed out", is_error=True))
            except Exception as e:
                _ok(mid, tool_result(f"{name}: {type(e).__name__}: {e}", is_error=True))
            return
        if method == "resources/list":
            _ok(mid, {"resources": resources_list()})
            return
        if method == "resources/templates/list":
            _ok(mid, {"resourceTemplates": []})
            return
        if method == "resources/read":
            _ok(mid, resource_read(params.get("uri", "")))
            return
        if method == "prompts/list":
            _ok(mid, {"prompts": PROMPTS})
            return
        if method == "prompts/get":
            _ok(mid, prompt_get(params.get("name", ""), params.get("arguments")))
            return
        _err(mid, -32601, f"method not found: {method}")
    except FileNotFoundError as e:
        _err(mid, -32002, str(e))
    except Exception as e:
        _err(mid, -32603, f"{type(e).__name__}: {e}")


def main() -> None:
    os.chdir(ROOT)
    LOGS.mkdir(exist_ok=True)
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    while True:
        msg = _read_message()
        if msg is None:
            break
        handle(msg)


if __name__ == "__main__":
    main()
