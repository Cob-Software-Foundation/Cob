#!/usr/bin/env python3
"""Cob Soft: the site's display face, a static cut of Fraunces (c) The Fraunces Project Authors, SIL OFL 1.1.

Fraunces is a variable "Old Style soft-serif" with four axes. Cob Soft pins them to a buttery display setting:
  SOFT 100 (rounded corners and terminals), WONK 1 (the leaning n/m/h and friendly italic forms),
  opsz 72 (display optical size), wght 600 (roman) / 560 (italic).
The result is renamed (it is a modified version), subset to Latin, and written as woff2 + ttf.

Run:  python3 tools/fonts/build_cob_soft.py     (needs: fonttools brotli; downloads Fraunces from google/fonts once)
"""
import pathlib, urllib.request
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

BASE = "https://raw.githubusercontent.com/google/fonts/main/ofl/fraunces/"
SRC = {"Regular": "Fraunces%5BSOFT%2CWONK%2Copsz%2Cwght%5D.ttf", "Italic": "Fraunces-Italic%5BSOFT%2CWONK%2Copsz%2Cwght%5D.ttf"}
AXES = {"Regular": dict(opsz=72, wght=600, SOFT=100, WONK=1), "Italic": dict(opsz=72, wght=560, SOFT=100, WONK=1)}
UNICODES = [*range(0x20, 0x7F), *range(0xA0, 0x100), 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2032, 0x2033, 0x20AC, 0x2122]
OUT = pathlib.Path(__file__).resolve().parents[2] / "docs" / "fonts"
CACHE = pathlib.Path("/tmp/cob-soft-src")

def build(style):
    CACHE.mkdir(exist_ok=True)
    src = CACHE / SRC[style].replace("%5B", "[").replace("%5D", "]").replace("%2C", ",")
    if not src.exists(): src.write_bytes(urllib.request.urlopen(BASE + SRC[style]).read())
    f = instancer.instantiateVariableFont(TTFont(src), AXES[style], inplace=False)
    ital = style == "Italic"
    ps = "CobSoft-" + style
    for rec in list(f["name"].names):
        if rec.nameID in (1, 2, 3, 4, 6, 16, 17, 21, 22, 25): f["name"].removeNames(nameID=rec.nameID)
    for nid, val in ((1, "Cob Soft"), (2, style), (3, "Cob Soft " + style + " 1.000"), (4, "Cob Soft " + style), (6, ps)):
        f["name"].setName(val, nid, 3, 1, 0x409); f["name"].setName(val, nid, 1, 0, 0)
    f["OS/2"].usWeightClass = 600 if not ital else 560
    f["OS/2"].fsSelection = (f["OS/2"].fsSelection & ~0x61) | (0x01 if ital else 0x40)
    f["head"].macStyle = 2 if ital else 0
    f["post"].italicAngle = f["post"].italicAngle if ital else 0
    opts = subset.Options(); opts.layout_features = ["*"]; opts.name_IDs = ["*"]; opts.notdef_outline = True; opts.glyph_names = False
    sub = subset.Subsetter(opts); sub.populate(unicodes=UNICODES); sub.subset(f)
    OUT.mkdir(parents=True, exist_ok=True)
    f.save(OUT / f"cob-soft-{style.lower()}.ttf")
    w = TTFont(OUT / f"cob-soft-{style.lower()}.ttf"); w.flavor = "woff2"; w.save(OUT / f"cob-soft-{style.lower()}.woff2")

if __name__ == "__main__":
    for s in SRC: build(s)
