#!/usr/bin/env python3
"""Draws app/vrupscale.ico: the app's headset character (see DrawScene in app/ui.cpp), standing.

Needs: pip install pillow
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DARK, BODY, MID, SKY, BLUE = (38, 38, 38), (56, 58, 57), (79, 79, 79), (224, 224, 224), (29, 173, 255)


def draw(size, simple):
    k = 8   # supersampling
    S = size * k
    im = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    u = S / 256.0
    P = lambda *v: [x * u for x in v]
    lw = (14 if simple else 9) * u
    # legs and feet (blue)
    for x0, x1 in ((96, 88), (160, 172)):
        d.line(P(x0, 168, x1, 226), fill=BLUE, width=int(lw * 1.7))
        d.line(P(x1 - 6, 232, x1 + 26, 232), fill=BLUE, width=int(lw * 2.0))
        for cx in (x1 - 6, x1 + 26):
            r = lw
            d.ellipse([cx * u - r, 232 * u - r, cx * u + r, 232 * u + r], fill=BLUE)
    # strap band over the top
    d.arc(P(52, 18, 204, 150), 200, 340, fill=DARK, width=int(lw * 2.6))
    d.arc(P(52, 18, 204, 150), 203, 337, fill=MID, width=int(lw * 1.2))
    # side arms
    for x in (6, 214):
        d.rounded_rectangle(P(x, 92, x + 36, 120), radius=10 * u, fill=MID, outline=DARK, width=int(lw * 0.8))
    # body with a nose cut-out
    d.rounded_rectangle(P(22, 52, 234, 176), radius=40 * u, fill=BODY, outline=DARK, width=int(lw))
    d.ellipse(P(104, 150, 152, 200), fill=(0, 0, 0, 0), outline=None)
    nose = Image.new('L', (S, S), 0)
    ImageDraw.Draw(nose).ellipse(P(100, 148, 156, 204), fill=255)
    im.paste((0, 0, 0, 0), (0, 0), nose)
    d.arc(P(100, 148, 156, 204), 180, 360, fill=DARK, width=int(lw))
    # visor, glare, eyes
    d.rounded_rectangle(P(40, 68, 216, 146), radius=26 * u, fill=DARK)
    if not simple:
        d.line(P(166, 62, 136, 152), fill=MID, width=int(16 * u))
        d.line(P(190, 62, 160, 152), fill=MID, width=int(6 * u))
    for cx in (92, 164):
        r = (17 if simple else 14) * u
        cy = 106 * u
        d.ellipse([cx * u - r, cy - r, cx * u + r, cy + r], fill=MID if not simple else SKY, outline=DARK, width=int(lw * 0.5))
        if not simple:
            h = r * 0.3
            d.ellipse([cx * u - r * 0.35 - h, cy - r * 0.35 - h, cx * u - r * 0.35 + h, cy - r * 0.35 + h], fill=SKY)
    return im.resize((size, size), Image.LANCZOS)


if __name__ == '__main__':
    sizes = [16, 24, 32, 48, 64, 256]
    imgs = [draw(s, s <= 24) for s in sizes]
    out = os.path.join(ROOT, 'app', 'vrupscale.ico')
    imgs[-1].save(out, format='ICO', sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
    preview = Image.new('RGBA', (sum(sizes) + 10 * len(sizes), 256), SKY + (255,))
    x = 0
    for s, im in zip(sizes, imgs):
        preview.paste(im, (x, 256 - s), im)
        x += s + 10
    preview.save(os.environ.get('ICON_PREVIEW', os.devnull) if os.environ.get('ICON_PREVIEW') else os.path.join(ROOT, 'build', 'icon_preview.png'))
    print('wrote', out)
