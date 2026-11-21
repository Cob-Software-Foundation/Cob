#!/usr/bin/env python3
"""Regenerates installer/assets/*.png from assets/source/cob-logo.png (needs Pillow).

  icon.png     256x256  window icon: the logo tile with rounded corners
  logo.png     128x128  the corn alone, transparent, shown next to page titles
  sidebar.png  330x770  the left banner, drawn at 2x for hi-dpi (165x385 logical)
  Cob.ico      16..256  the Windows file/taskbar icon (embedded by build.rs)
  Cob.icns     16..512  the macOS app icon (used by tools/package_macos.sh)

The words on the banner ("Cob", the version, the platform) are NOT in the
image: the wizard draws them live so they can never go stale.
"""
from collections import deque
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent.parent
SRC = HERE / "assets" / "source" / "cob-logo.png"
OUT = HERE / "assets"

src = Image.open(SRC).convert("RGB")
W, H = src.size
px = src.load()

# ---- cut the corn out of its gradient backdrop -----------------------------------
# A plain flood fill cannot cross a gradient, so compare each pixel with its
# *neighbour*: the fill follows the smooth backdrop and stops at the corn's
# dark outline, where the step between neighbours is large.
TOL = 14
seen = [[False] * W for _ in range(H)]
q = deque()
for x in range(W):
    for y in (0, H - 1):
        seen[y][x] = True
        q.append((x, y))
for y in range(H):
    for x in (0, W - 1):
        if not seen[y][x]:
            seen[y][x] = True
            q.append((x, y))
while q:
    x, y = q.popleft()
    for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
        if 0 <= nx < W and 0 <= ny < H and not seen[ny][nx]:
            if max(abs(px[x, y][i] - px[nx, ny][i]) for i in range(3)) <= TOL:
                seen[ny][nx] = True
                q.append((nx, ny))
alpha = Image.new("L", (W, H), 255)
ap = alpha.load()
for y in range(H):
    for x in range(W):
        if seen[y][x]:
            ap[x, y] = 0
# trim one pixel (removes the green fringe the backdrop leaves on the outline), then soften
alpha = alpha.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.6))
corn = src.convert("RGBA")
corn.putalpha(alpha)
corn = corn.crop(alpha.point(lambda v: 255 if v > 24 else 0).getbbox())


def fit(img, box):
    """img scaled (smoothly) to fit inside a box x box square, centred on transparency."""
    s = min(box / img.width, box / img.height)
    img = img.resize((round(img.width * s), round(img.height * s)), Image.LANCZOS)
    sq = Image.new("RGBA", (box, box), (0, 0, 0, 0))
    sq.paste(img, ((box - img.width) // 2, (box - img.height) // 2), img)
    return sq


# ---- icon: the original tile, rounded -------------------------------------------
tile = src.convert("RGBA").resize((256, 256), Image.LANCZOS)
mask = Image.new("L", (256, 256), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, 255, 255], radius=52, fill=255)
tile.putalpha(mask)
tile.save(OUT / "icon.png")

# ---- Windows .ico: the rounded tile at every size Explorer asks for --------------
tile.save(OUT / "Cob.ico", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])

# ---- macOS .icns: macOS icon art sits inside a safe area (~80%) of a transparent canvas
mac = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
art = src.convert("RGBA").resize((412, 412), Image.LANCZOS)
amask = Image.new("L", (412, 412), 0)
ImageDraw.Draw(amask).rounded_rectangle([0, 0, 411, 411], radius=88, fill=255)
art.putalpha(amask)
mac.alpha_composite(art, (50, 50))
mac.save(OUT / "Cob.icns")

# ---- small title logo ------------------------------------------------------------
fit(corn, 128).save(OUT / "logo.png")

# ---- sidebar ---------------------------------------------------------------------
BW, BH = 330, 770
top, bot = (60, 184, 66), (16, 66, 24)          # the logo's greens
banner = Image.new("RGBA", (BW, BH))
d = ImageDraw.Draw(banner)
for y in range(BH):
    t = y / (BH - 1)
    d.line([(0, y), (BW, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,))

# soft golden glow behind the corn, like the logo's own backdrop
glow = Image.new("RGBA", (BW, BH), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([BW // 2 - 150, 120, BW // 2 + 150, 420], fill=(255, 224, 80, 120))
banner = Image.alpha_composite(banner, glow.filter(ImageFilter.GaussianBlur(46)))

# two faint diagonal facets
over = Image.new("RGBA", (BW, BH), (0, 0, 0, 0))
od = ImageDraw.Draw(over)
od.polygon([(0, BH), (BW, BH - 250), (BW, BH)], fill=(0, 0, 0, 46))
od.polygon([(0, 0), (BW, 0), (BW, 120), (0, 200)], fill=(255, 255, 255, 18))
banner = Image.alpha_composite(banner, over)

big = fit(corn, 250)
banner.alpha_composite(big, ((BW - 250) // 2, 150))
banner.convert("RGB").save(OUT / "sidebar.png", optimize=True)
print("wrote", *(p.name for p in sorted(OUT.iterdir()) if p.is_file()))
