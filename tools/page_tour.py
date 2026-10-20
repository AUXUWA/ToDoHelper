#!/usr/bin/env python3
"""Walk every page on the device and print the heap after each one.

Usage:
    python tools/page_tour.py [--host 192.168.137.232] [--rounds 3]

The device must be reachable and the debug shell enabled.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request

PAGES = [
    "home", "todo", "todo_detail", "weather", "calendar",
    "word", "ai_chat", "settings", "wifi", "splash",
]


def run(host: str, cmd: str, timeout: float = 20.0) -> str:
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


def summarize(out: str) -> str:
    fields = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "internal" and parts[1] in ("free", "largest", "int-low"):
            fields[parts[1]] = parts[2]
        elif len(parts) >= 3 and parts[0] == "psram" and parts[1] == "free":
            fields["psram"] = parts[2]
    return " | ".join(f"{k}={v}" for k, v in fields.items())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.137.232")
    ap.add_argument("--rounds", type=int, default=3)
    args = ap.parse_args()

    print(f"=== baseline ===\n{summarize(run(args.host, 'heap'))}")

    for rnd in range(1, args.rounds + 1):
        print(f"=== round {rnd} ===")
        for page in PAGES:
            t0 = time.time()
            run(args.host, f"page {page}")
            time.sleep(1.2)  # let the page settle and draw
            out = run(args.host, "heap")
            ft = run(args.host, "ftstats")
            dt = (time.time() - t0) * 1000
            counts = " ".join(ft.split("\n")[2:4]) if len(ft.split("\n")) > 3 else ""
            print(f"{page:12s} {summarize(out)}  {counts}  ({dt:.0f} ms round trip)")

    print("=== final ===\n" + run(args.host, "heap"))
    print(run(args.host, "ftstats"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
