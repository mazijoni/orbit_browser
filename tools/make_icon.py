"""Generates resources/orbit.ico (the Orbit mark: a ring with a satellite).

Usage: python tools/make_icon.py
Requires Pillow. The generated .ico is committed; rerun only when the mark changes.
"""
import math
import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
BG_TOP = (36, 34, 44, 255)
BG_BOTTOM = (16, 16, 18, 255)
RING = (236, 236, 240)
SATELLITE = (124, 140, 255)


def render(size: int) -> Image.Image:
    scale = 8  # supersample, then downscale for clean anti-aliasing
    s = size * scale
    # Rounded-square tile with a soft vertical gradient.
    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    grad = Image.new("RGBA", (1, s))
    for y in range(s):
        t = y / (s - 1)
        grad.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))
    grad = grad.resize((s, s))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.24), fill=255)
    tile.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(tile)
    c = s / 2
    r = s * 0.27
    width = max(int(s * (0.085 if size <= 24 else 0.065)), 1)
    d.ellipse([c - r, c - r, c + r, c + r], outline=RING + (255,), width=width)
    # Satellite on the ring, upper right.
    a = math.radians(-40)
    sx, sy = c + r * math.cos(a), c + r * math.sin(a)
    sr = s * (0.1 if size <= 24 else 0.085)
    d.ellipse([sx - sr * 1.45, sy - sr * 1.45, sx + sr * 1.45, sy + sr * 1.45],
              fill=BG_TOP)  # gap in the ring around the satellite
    d.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=SATELLITE + (255,))
    return tile.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = ROOT / "resources" / "orbit.ico"
    out.parent.mkdir(exist_ok=True)
    images = [render(sz) for sz in SIZES]
    images[-1].save(out, format="ICO", sizes=[(sz, sz) for sz in SIZES],
                    append_images=images[:-1])
    images[-1].save(ROOT / "resources" / "orbit.png")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
