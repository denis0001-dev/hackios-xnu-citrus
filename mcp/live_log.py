#!/usr/bin/env python3
"""Follow the phone kernel log while a run is live: ./xl log every 2 s, print only new bytes.

Stops when fastboot result is no longer 'running'. Used by the MCP start_log_follower tool
and can be run directly: python3 mcp/live_log.py [--out logs/foo.live]
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def env() -> dict[str, str]:
    e = os.environ.copy()
    e.setdefault("XL_DEVICE", "citrus")
    e["FB_TIMEOUT"] = "20"
    return e


def fetch_log() -> bytes | None:
    try:
        subprocess.run(
            [str(ROOT / "xl"), "log"],
            cwd=str(ROOT),
            env=env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
            check=False,
        )
        path = ROOT / "logs" / "loader.log"
        return path.read_bytes() if path.is_file() else None
    except (subprocess.TimeoutExpired, OSError):
        return None


def result_line() -> str:
    try:
        out = subprocess.run(
            ["fastboot", "getvar", "result"],
            cwd=str(ROOT),
            env=env(),
            capture_output=True,
            timeout=15,
            check=False,
        )
        blob = (out.stderr or b"") + (out.stdout or b"")
        for line in blob.splitlines():
            if line.startswith(b"result:"):
                return line.decode("ascii", "replace")
        return blob.decode("ascii", "replace")
    except (subprocess.TimeoutExpired, OSError):
        return ""


def emit_delta(cur: bytes, prev: bytes, sink) -> bytes:
    if prev:
        tail = prev[-4096:]
        at = cur.rfind(tail)
        delta = cur[at + len(tail) :] if at >= 0 else b"\n--- (log moved on) ---\n" + cur
    else:
        delta = cur
    if delta:
        sink.buffer.write(delta)
        sink.buffer.flush()
    return cur


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", type=Path, help="Also append the full fetched log here each poll.")
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)

    sys.stdout.write("== following kernel log; stops when the run ends ==\n")
    sys.stdout.flush()
    while True:
        r = result_line()
        if "running" in r:
            break
        if r.strip():
            sys.stdout.write(f"waiting: {r.strip()}\n")
            sys.stdout.flush()
        time.sleep(args.interval)

    prev = b""
    while True:
        r = result_line()
        cur = fetch_log()
        if cur:
            if args.out:
                args.out.write_bytes(cur)
            emit_delta(cur, prev, sys.stdout)
            prev = cur
        if "running" not in r:
            sys.stdout.write("\n== the run is over ==\n")
            sys.stdout.flush()
            break
        time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
