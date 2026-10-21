#!/usr/bin/env python3
"""Measure what one *new* character costs to draw.

The number that matters
-----------------------
Drawing a character that is not in the FreeType glyph cache means
rasterising it: reading the glyph's tables out of the .ttf and rendering it.
That is the cost this script measures, and it is the cost that decides
whether a scrolling list feels smooth.

How to measure it without measuring something else
--------------------------------------------------
The debug shell's `bench` command times "set a label's text + redraw now".
That number contains three different things:

1. the glyph rasterisation (what we want);
2. the layout change, if the new text is a different length than the old
   one (the label re-wraps, the page re-layouts);
3. the flush of the panel, which on this board is **the whole frame**
   (800x480) and costs ~22 ms whenever it happens.

(2) and (3) are the same for every firmware, and (3) can be 20 ms bigger
than the thing we want to measure - so a raw `bench` number says almost
nothing about glyphs.

So this script measures the **difference** between two redraws of the *same*
label with the *same* length of text:

  * the first draw has to rasterise every character (they are not in the
    cache yet);
  * the second draw has them all cached;

  glyph cost = first - second

Everything else - layout, flush, panel - is in both measurements and
cancels out.

Usage:
    python tools/bench_font_ram.py [--host <设备IP>]

设备地址的取法：--host > 环境变量 TODOHELPER_HOST > 下面的通用示例。
"""

from __future__ import annotations

import argparse
import os
import json
import sys
import urllib.request

# The device's address: --host wins, then $TODOHELPER_HOST, then the generic
# example below. The default is a placeholder on purpose - the repo is public,
# so no real LAN address of the maintainer's is baked in.
DEFAULT_HOST = os.environ.get("TODOHELPER_HOST", "192.168.1.50")

# Ordinary GB2312 hanzi that are not used anywhere in the UI, so they are
# cold on a freshly booted device.
COLD_TEXT = (
    "白日依山尽黄河入海流欲穷千里目更上一层楼"
    "孤帆远影碧空尽唯见长江天际流"
    "青山横北郭白水绕东城此地一为别孤帆万里看"
    "海内存知己天涯若比邻无为在歧路儿女共沾巾"
)

# Groups of four characters each - used for the relative comparison
# (one group = one HTTP round trip, so the numbers are directly comparable
# between two firmwares only if both are measured the same way).
GROUPS = [
    "秦时明月",
    "汉时关万",
    "里长征人",
    "未还但使",
    "龙城飞将",
    "在不教胡",
    "马度阴山",
    "迟日江山",
    "丽春风花",
    "草香泥融",
]


def run(host: str, cmd: str, timeout: float = 300.0) -> str:
    body = json.dumps({"cmd": cmd}, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        f"http://{host}/api/v1/debug/exec",
        data=body,
        headers={"Content-Type": "application/json; charset=utf-8"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.loads(resp.read().decode())
    if payload.get("code") != 0:
        raise RuntimeError(f"device error: {payload}")
    return payload["data"]["out"]


def first_microseconds(out: str) -> int:
    for line in out.splitlines():
        if line.startswith("bench 1:"):
            return int(line.split(":", 1)[1].split()[0])
    raise RuntimeError(f"unexpected output: {out!r}")


def bench_text(host: str, text: str) -> int:
    return first_microseconds(run(host, f"bench label {text} 0 1"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--groups", action="store_true",
                        help="also print the per-group numbers (relative comparison)")
    args = parser.parse_args()

    # Warm-up: the first group changes the label's width, which moves the
    # clock label next to it in the status bar. Until that has settled, the
    # status bar is repainted on every redraw.
    bench_text(args.host, "预热预热")

    cold = bench_text(args.host, COLD_TEXT)
    warm = bench_text(args.host, COLD_TEXT)
    chars = len(COLD_TEXT)
    per_glyph = (cold - warm) / chars

    print(f"first draw (glyphs not cached) : {cold:>9} us")
    print(f"second draw (all cached)        : {warm:>9} us")
    print(f"=> rasterising {chars} glyphs    : {cold - warm:>9} us")
    print(f"=> per new glyph                : {per_glyph:>9.0f} us")

    if args.groups:
        total = 0
        for group in GROUPS:
            us = bench_text(args.host, group)
            total += us
            print(f"  {group}  {us:>9} us")
        print(f"groups total: {total} us")

    return 0


if __name__ == "__main__":
    sys.exit(main())
