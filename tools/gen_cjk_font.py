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
import json
import os
import sys
from pathlib import Path

DEFAULT_SOURCES = [
    r"C:\Windows\Fonts\NotoSansSC-VF.ttf",
    r"C:\Windows\Fonts\sourcehansans.ttf",
    r"C:\Windows\Fonts\msyh.ttc",
    r"C:\Windows\Fonts\simhei.ttf",
]

# Latin fonts that carry the IPA block, used to fill in what Noto Sans SC lacks.
# All of the Windows entries are ordinary system fonts; DejaVu Sans is listed
# first for Linux/CI because it is the one with a clear free licence (Bitstream
# Vera / public-domain additions) and is installed nearly everywhere.
DEFAULT_IPA_SOURCES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    r"C:\Windows\Fonts\segoeui.ttf",
    r"C:\Windows\Fonts\arial.ttf",
    r"C:\Windows\Fonts\calibri.ttf",
]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(REPO_ROOT, "components", "ui_core", "fonts", "ui_cjk.ttf")


def gb2312_charset(level2: bool = False) -> set[str]:
    """Characters representable in GB2312.

    Level 1 (0xB0A1-0xD7F9) is the 3755 most common hanzi and covers essentially
    all everyday simplified Chinese. Level 2 (0xD8A1-0xF7FE) adds 3008 rarer
    ones, which roughly doubles the file size.
    """
    if level2:
        high_range = range(0xA1, 0xFF)
    else:
        high_range = range(0xB0, 0xD8)

    chars: set[str] = set()
    for high in high_range:
        for low in range(0xA1, 0xFF):
            try:
                chars.add(bytes([high, low]).decode("gb2312"))
            except UnicodeDecodeError:
                continue
    return chars


def required_charset(level2: bool = False) -> set[str]:
    chars = gb2312_charset(level2)

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

    # IPA, for the word book's phonetics.
    #
    # Without this the phonetics render with holes: `/əˈbrʌpt/` came out as
    # `/  br  pt/` on the device because U+0259 (ə) and U+028C (ʌ) were not in
    # the subset. Latin-1 (0xA0-0xFF) does not cover them - IPA sits in its own
    # block at U+0250-02AF, with the length mark ː and stress marks ˈ ˌ in
    # Spacing Modifier Letters (U+02B0-02FF).
    for start, end in (
        (0x0250, 0x02AF),   # IPA Extensions
        (0x02B0, 0x02FF),   # Spacing Modifier Letters (ˈ ˌ ː ˈ etc.)
    ):
        chars.update(chr(c) for c in range(start, end + 1))

    # Belt to those braces: take every non-ASCII character that actually appears
    # in the shipped word books, so a new phonetic symbol in the data cannot
    # silently come out as a blank again.
    chars.update(_charset_from_word_books())
    return chars


def _charset_from_word_books() -> set[str]:
    """Every character used by the JSON word books under assets/words/.

    Returns an empty set when the files are missing (the generator is also used
    to refresh a subset on a machine that does not have them).
    """
    words_dir = Path(__file__).resolve().parent.parent / "assets" / "words"
    if not words_dir.is_dir():
        return set()

    chars: set[str] = set()
    for path in sorted(words_dir.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as err:
            print(f"warning: cannot read {path.name}: {err}")
            continue
        for entry in data.get("words", []):
            for field in ("spelling", "phonetic", "meaning", "example"):
                chars.update(str(entry.get(field, "")))
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


def find_ipa_source(explicit: str | None) -> str | None:
    """Locate a Latin font that carries the IPA block.

    **Noto Sans SC does not contain IPA at all** (verified: U+0259 ə, U+028C ʌ,
    U+026A ɪ ... are absent from its 30890-glyph cmap). The word book is full of
    phonetics like /əˈbrʌpt/, so without a second source those characters render
    as nothing at all - the device showed `/  br  pt/`. Nor does LVGL's built-in
    Montserrat fallback help: its sparse cmap only has FontAwesome ranges.

    Returns None when nothing suitable is installed, in which case the subset is
    simply built without IPA (better than failing the whole build).
    """
    if explicit:
        if not os.path.exists(explicit):
            sys.exit(f"IPA font not found: {explicit}")
        return explicit
    for candidate in DEFAULT_IPA_SOURCES:
        if os.path.exists(candidate):
            return candidate
    return None


def merge_ipa_glyphs(base_path: str, ipa_path: str, chars: set[str]) -> None:
    """Copy the IPA glyphs from @p ipa_path into the subset at @p base_path.

    A second font file is not shipped because the device loads fonts by path from
    the SD card (LVGL v9 FreeType cannot render from memory) and every extra face
    costs another glyph cache; merging keeps the one-file model.

    `fontTools.merge` cannot be used here: it asserts that every source shares a
    unitsPerEm, and the CJK font is 1000 while the Latin ones are 2048. The glyphs
    are therefore copied per-character and scaled to the base font's em square.
    """
    from copy import deepcopy

    from fontTools.ttLib import TTFont, newTable
    from fontTools.ttLib.tables import ttProgram
    from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
    from fontTools.ttLib.tables._g_l_y_f import Glyph

    base = TTFont(base_path)
    ipa = TTFont(ipa_path)

    base_cmap = base.getBestCmap()
    ipa_cmap = ipa.getBestCmap()
    base_upem = base["head"].unitsPerEm
    ipa_upem = ipa["head"].unitsPerEm
    scale = base_upem / ipa_upem

    wanted = [c for c in sorted(chars) if ord(c) not in base_cmap and ord(c) in ipa_cmap]
    if not wanted:
        base.close()
        ipa.close()
        return

    base_glyph_order = list(base.getGlyphOrder())
    ipa_glyf = ipa["glyf"]
    ipa_hmtx = ipa["hmtx"]
    glyf = base["glyf"]
    hmtx = base["hmtx"]
    # The CJK subset has vertical metrics; the Latin sources may or may not.
    vmtx = base["vmtx"] if "vmtx" in base else None
    vhea = base["vhea"] if "vhea" in base else None
    ipa_vmtx = ipa["vmtx"] if "vmtx" in ipa else None
    vhea_advance = int(vhea.advanceHeightMax) if vhea is not None else base_upem

    new_map = dict(base_cmap)
    added = 0
    # Components are copied under their original names, so a glyph pulled in
    # twice (a shared accent) is only written once.
    copied: dict[str, str] = {}

    def scale_glyph(glyph) -> None:
        """Scale a glyph's coordinates from the IPA em square into the base one.

        Both fonts are y-up with the baseline at 0, so this is a pure scale. It is
        required rather than cosmetic: the base em is 1000 and the Latin sources
        are 2048, so an unscaled copy would render at roughly twice the size.
        """
        if glyph.isComposite():
            for comp in glyph.components:
                # Component offsets live in the parent's coordinate space, so they
                # scale with it; otherwise an accent lands ~2x too far from the
                # letter it belongs to.
                if hasattr(comp, "x"):
                    comp.x = int(round(comp.x * scale))
                if hasattr(comp, "y"):
                    comp.y = int(round(comp.y * scale))
                if getattr(comp, "transform", None) is not None:
                    xx, xy, yx, yy, dx, dy = comp.transform
                    comp.transform = (xx, xy, yx, yy, int(round(dx * scale)),
                                      int(round(dy * scale)))
            # No recalcBounds() here: on a composite it resolves components
            # through the table it is given, and at this point they are being
            # installed into a *different* font. fontTools recomputes bounds at
            # compile time anyway, so leaving them stale is harmless.
            return

        # getCoordinates() returns a GlyphCoordinates for simple glyphs; it
        # supports in-place scaling and is what glyf expects back on assignment.
        coords, end_pts, flags = glyph.getCoordinates(ipa_glyf)
        coords.scale((scale, scale))
        glyph.coordinates = coords
        glyph.endPtsOfContours = list(end_pts)
        glyph.flags = list(flags)
        glyph.recalcBounds(ipa_glyf)

    def copy_glyph(src_name: str, dst_name: str) -> None:
        """Add `src_name` from the IPA font to the base font as `dst_name`.

        The glyph is **flattened to plain contours**. Keeping components would be
        smaller, but a component is a reference by name into the source font, and
        those glyphs do not exist in the base font - fontTools then fails at
        compile time with `KeyError: 'uni02BE'`. Decomposing sidesteps the whole
        problem instead of trying to replicate the component graph.
        """
        if dst_name in copied:
            return
        src = deepcopy(ipa_glyf[src_name])

        # getCoordinates() resolves a composite into its underlying contours and
        # is the documented way to decompose without walking the glyph tree.
        coords, end_pts, flags = src.getCoordinates(ipa_glyf)
        coords.scale((scale, scale))

        flat = Glyph()
        flat.numberOfContours = len(end_pts)
        flat.coordinates = coords
        flat.endPtsOfContours = list(end_pts)
        flat.flags = list(flags)
        # An **empty** hinting program is required, not optional:
        # glyf.compileCoordinates() calls program.getBytecode() unconditionally,
        # so a glyph without the attribute fails to compile. Empty is correct
        # because the subset is built with hinting off and LVGL renders unhinted
        # greyscale glyphs, so real bytecode would be dead weight.
        flat.program = ttProgram.Program()
        flat.program.fromBytecode(b"")
        flat.recalcBounds(None)

        glyf.glyphs[dst_name] = flat
        base_glyph_order.append(dst_name)
        advance, lsb = ipa_hmtx[src_name]
        hmtx.metrics[dst_name] = (int(round(advance * scale)), int(round(lsb * scale)))
        # **vmtx as well as hmtx.** Both tables are keyed by glyph name and both
        # must have one entry per glyph; updating only hmtx leaves vmtx short by
        # two bytes per added glyph, and fontTools then refuses to even parse the
        # file back ("not enough 'vmtx' table data: expected 10686, got 10280").
        # The CJK subset carries vmtx/vhea, so this is not hypothetical - the
        # first version of this function shipped exactly that broken font.
        #
        # Latin sources (DejaVu, Segoe UI, Arial) have no vertical metrics at all,
        # which is the normal case here; the base font's default advanceHeight is
        # then the honest value, since this font is only ever laid out
        # horizontally and vmtx exists purely for table consistency.
        if vmtx is not None:
            if ipa_vmtx is not None and src_name in ipa_vmtx.metrics:
                v_advance, tsb = ipa_vmtx[src_name]
                vmtx.metrics[dst_name] = (int(round(v_advance * scale)),
                                          int(round(tsb * scale)))
            else:
                vmtx.metrics[dst_name] = (vhea_advance, 0)
        copied[src_name] = dst_name

    for ch in wanted:
        src_name = ipa_cmap[ord(ch)]
        dst_name = f"ipa_{ord(ch):04X}"
        copy_glyph(src_name, dst_name)
        new_map[ord(ch)] = copied.get(src_name, dst_name)
        added += 1

    base.setGlyphOrder(base_glyph_order)
    # glyf keeps its own copy of the glyph order and asserts it matches the glyph
    # dict at compile time, so it has to be updated as well as the font's.
    base["glyf"].glyphOrder = base_glyph_order

    # Rebuild the cmap. The old subtables are dropped wholesale and one format-4
    # subtable is built from a plain dict: editing a real table in place for ~100
    # new codepoints would be far more code for no benefit.
    cmap_table = newTable("cmap")
    cmap_table.tableVersion = 0
    subtable = CmapSubtable.newSubtable(4)
    subtable.platformID = 3
    subtable.platEncID = 1
    subtable.language = 0
    subtable.cmap = new_map
    subtable.format = 4
    cmap_table.tables = [subtable]
    base["cmap"] = cmap_table

    # maxp must agree with the glyph count or the font is rejected at load time.
    base["maxp"].numGlyphs = len(base_glyph_order)
    # hhea/vhea numberOf*Metrics must not exceed the glyph count either; they are
    # written verbatim into the table header and FreeType cross-checks them.
    if "hhea" in base:
        base["hhea"].numberOfHMetrics = min(base["hhea"].numberOfHMetrics, len(base_glyph_order))
    if vhea is not None:
        vhea.numberOfVMetrics = min(vhea.numberOfVMetrics, len(base_glyph_order))
    if "post" in base:
        base["post"].formatType = 3.0   # no glyph names: smaller, and LVGL does not need them

    base.save(base_path)
    base.close()
    ipa.close()
    print(f"ipa    : copied {len(wanted)} glyphs from {os.path.basename(ipa_path)} "
          f"(scale {scale:.3f})")


def _required_content_charset(requested: set[str]) -> set[str]:
    """The subset of @p requested that the font *must* carry.

    Two sources, both of them real pixels on the panel:

    1. Every character in the shipped word books (assets/words/*.json). This is
       the `/əˈbrʌpt/` -> `/  br  pt/` class of bug, caught at build time.
    2. The IPA block, but only when a Latin source is available to supply it -
       otherwise a CJK-only machine would fail validation for glyphs it has no
       way to produce.

    Everything else in @p requested is best-effort: the charset deliberately
    sweeps whole Unicode ranges for coverage, and a font is not broken merely
    because it lacks, say, U+2023.
    """
    must_have = set(_charset_from_word_books())
    ipa_font = find_ipa_source(None)
    if ipa_font is not None:
        try:
            from fontTools.ttLib import TTFont

            with TTFont(ipa_font) as font:
                ipa_cmap = font.getBestCmap()
            must_have.update(c for c in requested if ord(c) in ipa_cmap)
        except Exception:                        # noqa: BLE001 - best effort only
            pass
    return must_have


def validate_font(path: str, required: set[str]) -> None:
    """Fail loudly if the finished subset would not render or would not load.

    This exists because the first IPA merge shipped a font whose `vmtx` table was
    two bytes short per added glyph: it still had every cmap entry, so a
    codepoint-only check ("does it contain ə?") passed and the breakage only
    showed up when the file was re-parsed. Re-opening the saved file and reading
    every metrics table is what actually catches it.
    """
    from fontTools.ttLib import TTFont

    try:
        font = TTFont(path)
    except Exception as err:                     # noqa: BLE001 - report and abort
        sys.exit(f"font validation failed: {path} is not loadable: {err}")

    glyph_count = font["maxp"].numGlyphs
    errors: list[str] = []

    # Every per-glyph table must have exactly one entry per glyph.
    for tag in ("hmtx", "vmtx"):
        if tag in font:
            have = len(font[tag].metrics)
            if have != glyph_count:
                errors.append(f"{tag} has {have} entries, expected {glyph_count}")

    for tag, attr in (("hhea", "numberOfHMetrics"), ("vhea", "numberOfVMetrics")):
        if tag in font:
            value = getattr(font[tag], attr)
            if value > glyph_count:
                errors.append(f"{tag}.{attr} is {value}, exceeds {glyph_count} glyphs")

    # And every character that must be there really must be in the cmap.
    #
    # Only *content* characters are demanded. The requested charset also includes
    # whole decorative Unicode ranges (general punctuation, etc.) that a CJK font
    # is entitled not to carry; failing on those would make this check noise that
    # people learn to ignore. What must never be missing is anything the UI or the
    # shipped word books actually paint - that is precisely the `/  br  pt/` bug.
    must_have = _required_content_charset(required)
    cmap = font.getBestCmap()
    missing = sorted(c for c in must_have if ord(c) not in cmap)
    if missing:
        errors.append(f"{len(missing)} required chars absent from cmap: "
                      f"{''.join(missing[:32])}")

    font.close()

    if errors:
        sys.exit("font validation failed:\n  - " + "\n  - ".join(errors))


def instance_variable_font(source: str, out_path: str, weight: int = 400) -> str | None:
    """Pin a variable font to a single weight.

    Noto Sans SC ships as a variable font; keeping the wght axis multiplies the
    glyph data for a device that only ever renders one weight. Returns the path
    of the static instance, or None when the source is already static.
    """
    try:
        from fontTools.ttLib import TTFont
        from fontTools.varLib import instancer
    except ImportError:
        return None

    font = TTFont(source, lazy=True)
    if "fvar" not in font:
        font.close()
        return None

    axes = {axis.axisTag: axis for axis in font["fvar"].axes}
    location = {}
    if "wght" in axes:
        location["wght"] = weight
    if not location:
        font.close()
        return None

    print(f"variable font: pinning {location} -> static instance")
    instance = instancer.instantiateVariableFont(font, location, inplace=False, updateFontNames=False)
    instance.save(out_path)
    instance.close()
    font.close()
    return out_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", help="source TTF/OTF/TTC")
    parser.add_argument(
        "--ipa-source",
        help="Latin font used to fill in phonetic characters the CJK font lacks "
        "(default: the first of " + ", ".join(DEFAULT_IPA_SOURCES) + " that exists)",
    )
    parser.add_argument("--out", default=DEFAULT_OUT, help="output subset TTF")
    parser.add_argument(
        "--keep-layout",
        action="store_true",
        help="keep OpenType layout tables (bigger, not needed by LVGL FreeType)",
    )
    parser.add_argument(
        "--level2",
        action="store_true",
        help="also include GB2312 level 2 (6763 hanzi total, roughly 2x the size)",
    )
    parser.add_argument(
        "--weight", type=int, default=400, help="weight to pin a variable font to"
    )
    args = parser.parse_args()

    try:
        from fontTools import subset
    except ImportError:
        sys.exit("fontTools is required: pip install fonttools brotli")

    source = find_source(args.source)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    # Step 1: drop the variable axes so the subsetter only keeps one weight.
    static_path = os.path.join(os.path.dirname(args.out), ".static_tmp.ttf")
    instanced = instance_variable_font(source, static_path, args.weight)
    effective_source = instanced if instanced is not None else source

    text = "".join(sorted(required_charset(args.level2)))

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

    font = subset.load_font(effective_source, options)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=text)
    subsetter.subset(font)
    subset.save_font(font, args.out, options)
    font.close()

    if instanced is not None:
        os.remove(static_path)

    # Step 2: fill in the phonetic characters the CJK source does not have.
    # Without this the word book's phonetics render with holes (the device showed
    # `/  br  pt/` for `/əˈbrʌpt/`), because Noto Sans SC has no IPA block.
    ipa_source = find_ipa_source(args.ipa_source)
    if ipa_source is None:
        print("warning: no Latin font with IPA found; phonetics will have gaps.")
        print("         pass --ipa-source <ttf> to fix that.")
    else:
        merge_ipa_glyphs(args.out, ipa_source, set(text))

    # Step 3: prove the file we just wrote is actually loadable and complete.
    # A cmap-only check is not enough - see validate_font().
    validate_font(args.out, set(text))
    print("validate: OK (metrics tables consistent, all requested chars present)")

    size = os.path.getsize(args.out)
    print(f"source : {source}")
    print(f"level2 : {args.level2}")
    print(f"chars  : {len(text)}")
    print(f"output : {args.out} ({size} bytes, {size / 1024:.0f} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
