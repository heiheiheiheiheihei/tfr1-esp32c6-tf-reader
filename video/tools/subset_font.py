#!/usr/bin/env python3
"""Subset Noto Sans CJK SC to the glyphs this film actually uses.

The system TTCs are 20 MB; a woff2 with only the ~200 characters on screen is
a few kB, so the project renders with no network at all.
"""
import glob
import sys

from fontTools import subset
from fontTools.ttLib import TTCollection

SRC = [
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2, "Regular", 400),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", 2, "Bold", 700),
]

text = set()
for f in glob.glob("index.html") + glob.glob("compositions/*.html") + glob.glob("*.md"):
    text |= set(open(f, encoding="utf-8").read())
# latin + punctuation the film may type in a Chinese run
text |= set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
text |= set(" ·—…、，。：；！？“”‘’（）《》/-+×%°→←↑↓▶■□●○·")
chars = "".join(sorted(c for c in text if c.isprintable()))

for path, idx, name, weight in SRC:
    coll = TTCollection(path, lazy=True)
    font = coll.fonts[idx]
    opts = subset.Options()
    opts.flavor = "woff2"
    opts.desubroutinize = True
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    sub = subset.Subsetter(options=opts)
    sub.populate(text=chars)
    sub.subset(font)
    out = f"assets/fonts/NotoSansSC-{name}.woff2"
    font.flavor = "woff2"
    font.save(out)
    print(f"{out}  weight={weight}  glyphs={len(chars)}")
