#!/usr/bin/env python3
"""Measure what one full-screen redraw costs - the cost of one scroll frame.

The debug shell's `bench-screen` command invalidates the whole screen and
redraws it synchronously, N times. The first redraw rasterises every
character that is not in the FreeType cache yet (read the TTF from the SD card
and render - about 9 ms per character); the rest are the **steady state**.

The steady state is the number that says whether the UI feels smooth: if the
glyph cache is too small to hold what a screen shows, every frame has to
rasterise the same characters again and the number stays high forever.

Usage:
    python tools/bench_glyphs.py [--host <设备IP>] [--page home]

设备地址的取法：--host > 环境变量 TODOHELPER_HOST > 下面的通用示例。
"""

from __future__ import annotations

import argparse
import os
import json
import sys
import time
import urllib.request

# The device's address: --host wins, then $TODOHELPER_HOST, then the generic
# example below. The default is a placeholder on purpose - the repo is public,
# so no real LAN address of the maintainer's is baked in.
DEFAULT_HOST = os.environ.get("TODOHELPER_HOST", "192.168.1.50")


def run(host: str, cmd: str, timeout: float = 60.0) -> str:
    # ensure_ascii=False keeps the hanzi as UTF-8 instead of \uXXXX escapes.
    body = json.dumps({"cmd": cmd}, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        f"http://{host}/api/v1/debug/exec",
        data=body,
        headers={"Content-Type": "application/json; charset=utf-8"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.loads(resp.read().decode())
    if payload.get("code") != 0:
        return f"ERROR: {payload}"
    return payload["data"]["out"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--page", default="home")
    ap.add_argument("--repeats", type=int, default=6)
    args = ap.parse_args()

    print(run(args.host, "ftstats").split("\n")[0])

    run(args.host, f"page {args.page}")
    time.sleep(1.5)

    out = run(args.host, f"bench-screen {args.repeats}")
    times = []
    for line in out.split("\n"):
        if line.startswith("bench "):
            times.append(int(line.split(":")[1].strip().split()[0]))

    if not times:
        print(f"FAILED: {out.strip()}")
        return 1

    for i, us in enumerate(times):
        print(f"redraw {i + 1}: {us:>8} us")
    if len(times) > 1:
        steady = times[1:]
        print(f"first (cold): {times[0]} us")
        print(f"steady state : {sum(steady) // len(steady)} us  (one scroll frame)")
    print(run(args.host, "ftstats"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
