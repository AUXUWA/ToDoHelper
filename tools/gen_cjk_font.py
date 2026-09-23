#!/usr/bin/env python3
"""Generate the CJK subset font embedded into components/ui_core.

Why a subset: a full CJK font is 8-20 MB, which would dominate the 8 MB app
partition. GB2312 level 1 + level 2 covers 6763 hanzi, i.e. essentially all
everyday simplified Chinese, and ASCII/Latin-1/punctuation is kept for numbers,
dates and units.

Usage
-----
    python tools/gen_cjk_font.py [--source PATH_TO_TTF] [--out PATH_TO_TTF]

Default source search order (first existing wins):
    C:/Windows/Fonts/NotoSansSC-VF.ttf
    C:/Windows/Fonts/sourcehansans.ttf
    C:/Windows/Fonts/simhei.ttf

Only redistribute the produced subset if the source font licence allows it.
Noto Sans SC and Source Han Sans are both SIL OFL 1.1, which does.
"""

from __future__ import annotations

import argparse
import os
import sys

DEFAULT_SOURCES = [
    r"C:\Windows\Fonts\NotoSansSC-VF.ttf",
    r"C:\Windows\Fonts\sourcehansans.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO_ROOT, "components", "ui_core", "fonts", "ui_cjk.ttf")


def gb2312_charset() -> set[str]:
    """Every character representable in GB2312 (6763 hanzi + symbols)."""
    chars: set[str] = set()
    for high in range(0xA1, 0xFF):
        for low in range(0xA1, 0xFF):
            try:
                chars.add(bytes([high, low]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def required_charset() -> set[str]:
    chars = gb2312_charset()

    # ASCII + Latin-1 supplement (degrees, accented letters in city names).
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars.update(chr(c) for c in range(0xA0, 0x100))

    # General punctuation, arrows, CJK symbols, fullwidth forms.
    for start, end in (
        (0x2000, 0x206F),
        (0x2190, 0x21FF),
        (0x2460, 0x24FF),
        (0x25A0, 0x25FF),
        (0x2600, 0x26FF),
        (0x3000, 0x303F),
        (0xFE30, 0xFE4F),
        (0xFF00, 0xFFEF),
    ):
        chars.update(chr(c) for c in range(start, end + 1))

    # A few symbols used by the UI that are not in the ranges above.
    chars.update("℃℉°×÷≈→←↑↓✓✗⚠●○■□★☆")
    return chars


def find_source(explicit: str | None) -> str:
    if explicit:
        if not os.path.exists(explicit):
            sys.exit(f"source font not found: {explicit}")
        return explicit
    for candidate in DEFAULT_SOURCES:
        if os.path.exists(candidate):
            return candidate
    sys.exit(
        "no source font found; pass --source with a TTF/OTF that covers Chinese.\n"
        "looked for: " + ", ".join(DEFAULT_SOURCES)
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", help="source TTF/OTF/TTC")
    parser.add_argument("--out", default=DEFAULT_OUT, help="output subset TTF")
    parser.add_argument(
        "--keep-layout",
        action="store_true",
        help="keep OpenType layout tables (bigger, not needed by LVGL FreeType)",
    )
    args = parser.parse_args()

    try:
        from fontTools import subset
    except ImportError:
        sys.exit("fontTools is required: pip install fonttools brotli")

    source = find_source(args.source)
    text = "".join(sorted(required_charset()))
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    options = subset.Options()
    options.flavor = None
    options.with_zopfli = False
    options.desubroutinize = False
    options.drop_tables += ["DSIG"]
    if not args.keep_layout:
        options.layout_features = []
        options.drop_tables += ["GSUB", "GPOS", "GDEF", "morx", "kerx"]
    # Keep hinting out of the image: LVGL renders unhinted greyscale glyphs.
    options.hinting = False

    font = subset.load_font(source, options)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=text)
    subsetter.subset(font)
    subset.save_font(font, args.out, options)
    font.close()

    size = os.path.getsize(args.out)
    print(f"source : {source}")
    print(f"chars  : {len(text)}")
    print(f"output : {args.out} ({size} bytes, {size / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
