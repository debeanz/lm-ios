"""Draws the iOS app icon (packaging/ios/AppIcon*.png): an original picture, no game art.

A dark mansion silhouette under a night sky with a moon, swept by a warm flashlight beam, with a
glassy highlight in the style of iOS 26 icons. Run with Pillow:  py tools/ios/make_icon.py
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

S = 1024
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "packaging", "ios")


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def sky():
    img = Image.new("RGBA", (S, S))
    top, bottom = (22, 14, 48), (58, 36, 92)
    px = img.load()
    for y in range(S):
        c = lerp(top, bottom, y / (S - 1))
        for x in range(S):
            px[x, y] = c + (255,)
    return img


def mansion(draw):
    col = (10, 8, 20, 255)
    base_y = 860
    # main hall, two wings, towers with pointed roofs
    draw.rectangle([250, 560, 774, base_y], fill=col)
    draw.polygon([(230, 570), (512, 400), (794, 570)], fill=col)
    for x0 in (150, 694):
        draw.rectangle([x0, 620, x0 + 180, base_y], fill=col)
        draw.polygon([(x0 - 15, 630), (x0 + 90, 545), (x0 + 195, 630)], fill=col)
    for x0 in (300, 664):
        draw.rectangle([x0, 430, x0 + 60, 600], fill=col)
        draw.polygon([(x0 - 12, 440), (x0 + 30, 330), (x0 + 72, 440)], fill=col)
    draw.rectangle([0, base_y, S, S], fill=col)
    # lit windows
    win = (255, 196, 92, 255)
    for x in (300, 380, 600, 680):
        draw.rectangle([x, 640, x + 36, 700], fill=win)
    for x in (190, 760):
        draw.rectangle([x, 680, x + 30, 730], fill=(255, 170, 70, 255))
    draw.rectangle([480, 740, 544, base_y], fill=(40, 26, 18, 255))


def moon(img):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse([700, 120, 880, 300], fill=(250, 244, 220, 255))
    d.ellipse([745, 105, 925, 285], fill=(0, 0, 0, 0))
    glow = layer.filter(ImageFilter.GaussianBlur(28))
    img.alpha_composite(glow)
    img.alpha_composite(layer)


def beam(img):
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    ox, oy = 90, 980
    ang, spread, length = math.radians(-38), math.radians(11), 1250
    pts = [(ox, oy)]
    for k in range(21):
        a = ang - spread + 2 * spread * k / 20
        pts.append((ox + math.cos(a) * length, oy + math.sin(a) * length))
    d.polygon(pts, fill=(255, 226, 140, 120))
    layer = layer.filter(ImageFilter.GaussianBlur(18))
    img.alpha_composite(layer)


def glass(img):
    # soft top highlight and a thin bright rim, like a glass tile
    layer = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.ellipse([-200, -620, S + 200, 420], fill=(255, 255, 255, 34))
    layer = layer.filter(ImageFilter.GaussianBlur(40))
    img.alpha_composite(layer)
    rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([6, 6, S - 7, S - 7], radius=230, outline=(255, 255, 255, 60), width=6)
    img.alpha_composite(rim.filter(ImageFilter.GaussianBlur(2)))


def main():
    img = sky()
    moon(img)
    d = ImageDraw.Draw(img)
    mansion(d)
    beam(img)
    glass(img)
    icon = img.convert("RGB")  # iOS icons are opaque squares; the system rounds the corners
    os.makedirs(OUT, exist_ok=True)
    icon.save(os.path.join(OUT, "AppIcon1024.png"))
    for size, name in ((120, "AppIcon60x60@2x.png"), (180, "AppIcon60x60@3x.png"), (152, "AppIcon76x76@2x.png"),
                       (167, "AppIcon83.5x83.5@2x.png")):
        icon.resize((size, size), Image.LANCZOS).save(os.path.join(OUT, name))
    print("icons written to", os.path.abspath(OUT))


if __name__ == "__main__":
    main()
