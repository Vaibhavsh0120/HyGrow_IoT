#!/usr/bin/env python3
r"""
Regenerates data/fonts/material-symbols-outlined.woff2 and
data/css/_icon-codepoints.css.

--- Why this file looks the way it does (read before touching it) ---

The app used to render icons by writing the icon's plain-text name (e.g.
"chevron_right") inside a span and relying on the font's 'liga' OpenType
ligature feature to substitute that spelled-out name for a single icon
glyph. That broke in production: the upstream variable font ships TWO
separate GSUB Feature records both tagged "liga" (one real, one an empty
placeholder used elsewhere for the FILL-axis glyph swap), and this
script's old Step 1 -- blanket-renaming every rlig/rclt feature to "liga"
to match this repo's original hand-built font -- collapsed them into an
ambiguous pair that browsers' shaping engines resolved to the empty one.
Result: 100% of icons fell back to literal fallback text (see PROGRESS.md
for the full root-cause writeup). A defensive uppercase-ASCII cmap
fallback (mapping A-Z to the same glyphs as a-z) meant that fallback text
rendered in shouty caps instead of quietly-wrong lowercase, which is why
broken icons showed up as e.g. "WATER_DROP".

This script now avoids GSUB/ligatures entirely. Material Symbols also
ships each icon at a Private Use Area (PUA) Unicode codepoint -- e.g.
water_drop is U+E798 -- as an alternative, simpler mapping: one cmap
lookup, no shaping engine involved, so this whole bug class (ligature
rules silently not firing) is structurally impossible. The app now
renders icons via `<span class="material-symbols-outlined" data-icon="water_drop"></span>`
with NO text content -- the actual glyph comes from a generated
`[data-icon="water_drop"]::before { content: "\e798"; }` CSS rule, written
directly into data/css/style.css between the ICON-CODEPOINTS:START/END
markers (see write_codepoints_css() below -- NOT a separate @import'd
file; @import only works as the first statement in a stylesheet, and
style.css already has @font-face rules ahead of where this needs to
live). The `data-icon` attribute keeps the icon name available in the
DOM (useful for debugging, and for any code path that reads it back out,
e.g. hg-secret-toggle's visibility/visibility_off swap), but it is never
what gets shaped/rendered -- so a font regression can no longer turn into
25 characters of literal text blowing out a layout.

Whenever you add a new icon anywhere in the app (index.html or a
dynamically-set data-icon assignment in app.js), add its name to
ICON_NAMES below and re-run this script. It regenerates both the font
and the CSS codepoint rules (in style.css) from that single list, then
shape-tests the result and REFUSES TO WRITE OUTPUT if anything looks
wrong -- see verify_codepoints_render() below. Don't hand-edit either
output.

Usage:
    pip install --break-system-packages fonttools brotli uharfbuzz
    npm pack @material-symbols/font-400@0.46.0
    tar xzf material-symbols-font-400-0.46.0.tgz
    python3 tools/build-icon-font.py package/material-symbols-outlined.woff2

Requires: fontTools, brotli (for woff2 output), uharfbuzz (for the
build-time regression check).
"""
import sys
import os

# Every icon name this app currently references (index.html's data-icon
# attributes + app.js's data-icon assignments, including tabsData.icons /
# bottomNavItems). Keep this list alphabetized and in sync with the
# codebase -- it IS the font's glyph set AND the CSS codepoint table.
ICON_NAMES = [
    "air",
    "arrow_downward",
    "check",
    "chevron_right",
    "close",
    "cloud",
    "content_copy",
    "delete_forever",
    "device_thermostat",
    "download",
    "error",
    "health_and_safety",
    "help",
    "info",
    "light_mode",
    "lock",
    "memory",
    "menu",
    "monitoring",
    "power_off",
    "restart_alt",
    "schedule",
    "science",
    "sensors",
    "settings",
    "settings_input_component",
    "terminal",
    "thermostat",
    "visibility",
    "visibility_off",
    "warning",
    "water_drop",
    "waves",
    "wifi",
]


def build_codepoint_map(font):
    """Map each ICON_NAMES entry to its upstream PUA codepoint.

    The upstream font's cmap has some glyphs reachable from more than one
    codepoint (harmless historical aliasing upstream). Pick the lowest
    codepoint deterministically so re-running this script always produces
    byte-identical output for the same input font.
    """
    cmap = font.getBestCmap()
    glyph_to_cps = {}
    for cp, glyphname in cmap.items():
        if 0xE000 <= cp <= 0xF8FF:  # Private Use Area
            glyph_to_cps.setdefault(glyphname, []).append(cp)

    mapping = {}
    missing = []
    for name in ICON_NAMES:
        cps = glyph_to_cps.get(name)
        if not cps:
            missing.append(name)
        else:
            mapping[name] = min(cps)

    if missing:
        print(
            f"ERROR: {len(missing)} icon name(s) have no PUA codepoint in "
            f"the source font: {missing}",
            file=sys.stderr,
        )
        print(
            "Check spelling against the upstream Material Symbols icon "
            "list, or confirm the source font actually includes them.",
            file=sys.stderr,
        )
        sys.exit(1)

    return mapping


def verify_codepoints_render(font_path, codepoint_map):
    """Build-time regression guard.

    Shape-tests every icon's codepoint against the freshly-built font with
    uharfbuzz (the same shaping engine real browsers use) and confirms it
    resolves to exactly one non-.notdef glyph. This is the check that
    would have caught the original ligature bug before it ever shipped --
    if it ever fails again (for this codepoint approach OR if this script
    is ever reverted to a ligature-based one), the build stops here
    instead of silently producing a broken font.

    Shaped against a decompressed (non-woff2) copy of the font: this
    sandbox's uharfbuzz build loads zero glyphs from a raw woff2 blob
    (hb.Face(...).glyph_count == 0 on woff2 input, confirmed empirically),
    but shapes correctly once the *exact same* font data is re-flavored to
    an uncompressed sfnt. Same cmap/glyf/GSUB tables either way -- woff2
    is purely a compression wrapper around them -- so this sidesteps a
    tooling gap here without weakening what's actually being verified.
    """
    import uharfbuzz as hb
    from fontTools.ttLib import TTFont

    ttfont = TTFont(font_path)
    glyph_order = ttfont.getGlyphOrder()

    decompressed_path = font_path + ".shapetest.ttf"
    decompressed = TTFont(font_path)
    decompressed.flavor = None
    decompressed.save(decompressed_path)

    with open(decompressed_path, "rb") as f:
        font_data = f.read()
    os.remove(decompressed_path)

    face = hb.Face(font_data)
    hb_font = hb.Font(face)
    hb.ot_font_set_funcs(hb_font)

    failures = []
    for name, cp in codepoint_map.items():
        buf = hb.Buffer()
        buf.add_str(chr(cp))
        buf.guess_segment_properties()
        hb.shape(hb_font, buf, {"liga": False})  # explicitly not relying on GSUB

        glyph_ids = [info.codepoint for info in buf.glyph_infos]
        if len(glyph_ids) != 1:
            failures.append(
                f"{name} (U+{cp:04X}): shaped to {len(glyph_ids)} glyphs, expected 1"
            )
            continue
        gid = glyph_ids[0]
        if gid == 0 or glyph_order[gid] == ".notdef":
            failures.append(f"{name} (U+{cp:04X}): resolved to .notdef")

    if failures:
        print(
            f"ERROR: {len(failures)}/{len(codepoint_map)} icon(s) failed "
            "the build-time shape test:",
            file=sys.stderr,
        )
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        print(
            "Refusing to write output. This is exactly the class of bug "
            "that broke the old ligature-based font -- see the module "
            "docstring and PROGRESS.md.",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Shape test passed: {len(codepoint_map)}/{len(codepoint_map)} icons OK.")


def write_codepoints_css(codepoint_map, style_css_path):
    """Writes the icon-name -> codepoint rules directly into style.css,
    replacing the block between the AUTO-GENERATED marker comments.

    Deliberately NOT a separate @import'd file: CSS only honors @import
    when it's the very first statement in a stylesheet (before any other
    rule, @font-face included) -- style.css already has several @font-face
    blocks ahead of where the icon rules need to live, so an @import there
    is silently ignored by the browser and no icon renders at all. Writing
    the rules inline sidesteps that spec quirk entirely, and also means
    the device only has to serve/parse one CSS file instead of two.
    """
    start_marker = "/* ICON-CODEPOINTS:START (auto-generated by tools/build-icon-font.py -- do not hand-edit between the markers) */"
    end_marker = "/* ICON-CODEPOINTS:END */"

    rule_lines = [start_marker]
    for name in ICON_NAMES:  # stable, alphabetized order
        cp = codepoint_map[name]
        rule_lines.append(f'[data-icon="{name}"]::before {{ content: "\\{cp:04x}"; }}')
    rule_lines.append(end_marker)
    new_block = "\n".join(rule_lines)

    with open(style_css_path, "r") as f:
        css = f.read()

    if start_marker in css and end_marker in css:
        pre = css.split(start_marker)[0]
        post = css.split(end_marker)[1]
        css = pre + new_block + post
    else:
        raise RuntimeError(
            f"Could not find ICON-CODEPOINTS markers in {style_css_path}. "
            "They should already exist (added once by hand) -- see the "
            ".material-symbols-outlined::before comment block in style.css."
        )

    with open(style_css_path, "w") as f:
        f.write(css)
    print(f"Wrote {len(codepoint_map)} icon rules into {style_css_path}")


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path-to-full-material-symbols-outlined.woff2>")
        sys.exit(1)
    src_path = sys.argv[1]

    from fontTools.ttLib import TTFont
    from fontTools import subset as ft_subset

    # Step 1: read the codepoint map from the untouched source font, before
    # any subsetting touches its cmap/GSUB.
    src_font = TTFont(src_path)
    codepoint_map = build_codepoint_map(src_font)

    # Step 2: subset by Unicode codepoint, not by ligature text. No GSUB
    # layout features are requested at all -- pyftsubset's --unicodes mode
    # keeps exactly the requested cmap entries and their glyphs, with no
    # ligature-closure risk (the old script's Step 1/Step 2 dance existed
    # specifically to bound GSUB closure blowup; that whole problem class
    # doesn't apply here since we never touch GSUB).
    unicodes = ",".join(f"{cp:04X}" for cp in codepoint_map.values())
    args = [
        src_path,
        f"--unicodes={unicodes}",
        "--glyph-names",
        "--layout-features=",  # explicitly drop all GSUB/GPOS features
        "--flavor=woff2",
        "--output-file=/tmp/_subset.woff2",
    ]
    ft_subset.main(args)

    # Step 3: build-time regression guard -- refuses to proceed if any
    # icon doesn't resolve to exactly one real glyph.
    verify_codepoints_render("/tmp/_subset.woff2", codepoint_map)

    # Step 4: only now, having passed verification, write the real outputs.
    out_font_path = "data/fonts/material-symbols-outlined.woff2"
    final = TTFont("/tmp/_subset.woff2")
    final.save(out_font_path)
    print(f"Wrote {out_font_path} ({os.path.getsize(out_font_path):,} bytes)")
    print(f"Glyphs: {len(final.getGlyphOrder())}")

    out_css_path = "data/css/style.css"
    write_codepoints_css(codepoint_map, out_css_path)


if __name__ == "__main__":
    main()
