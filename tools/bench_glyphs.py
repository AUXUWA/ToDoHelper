#!/usr/bin/env python3
"""Measure how long a redraw takes when it has to rasterise fresh glyphs.

Each round uses a *different* string of hanzi, so every round starts with an
empty glyph cache for those characters - which is what a scrolling list meets
when it brings new text into view.

Usage:
    python tools/bench_glyphs.py [--host 192.168.137.232] [--rounds 4]
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request

# 30 hanzi per string, none repeated between the strings.
STRINGS = [
    "今天天气不错适合出门散步跑步锻炼身体保持健康快乐每一天",
    "明天的会议安排在上午十点地点在三楼会议室请提前十分钟到场",
    "这个项目已经完成第一阶段剩下的是测试和文档还需要两周时间",
    "他说他会把文件发到邮箱里但是到现在我还没有收到那封邮件",
]


def run(host: str, cmd: str, timeout: float = 30.0) -> str:
    body = json.dumps({"cmd": cmd}).encode()
    req = urllib.request.Request(
        f"http://{host}/api/v1/debug/exec",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.loads(resp.read().decode())
    if payload.get("code") != 0:
        return f"ERROR: {payload}"
    return payload["data"]["out"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.137.232")
    ap.add_argument("--rounds", type=int, default=len(STRINGS))
    ap.add_argument("--target", default="待办清单")
    args = ap.parse_args()

    stats = run(args.host, "ftstats").split("\n")[0]
    print(stats)

    total = 0.0
    for i in range(min(args.rounds, len(STRINGS))):
        text = STRINGS[i]
        t0 = time.time()
        out = run(args.host, f"bench {args.target} {text} 0 1")
        wall = (time.time() - t0) * 1000
        us = 0
        for line in out.split("\n"):
            if line.startswith("bench 1:"):
                us = int(line.split(":")[1].strip().split()[0])
        total += us
        print(f"round {i + 1}: {us:>8} us  (wall {wall:.0f} ms)")

    print(f"average: {total / max(1, min(args.rounds, len(STRINGS))):.0f} us")
    print(run(args.host, "ftstats"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
