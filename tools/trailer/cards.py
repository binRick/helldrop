#!/usr/bin/env python3
"""Trailer cards as PNG layers (the mini's ffmpeg has no drawtext).
   cards.py <build/trailer>   -> cards/<card>_bg.png + <card>_<i>.png layers, short_frame.png, cap_<seg>.png
Backgrounds are blurred, darkened gameplay stills (cards/still_*.png, pulled by the caller)."""
import os, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

B = sys.argv[1]; C = os.path.join(B, "cards"); os.makedirs(C, exist_ok=True)
SUP = "/System/Library/Fonts/Supplemental/"
BLACK, MONO = SUP + "Arial Black.ttf", SUP + "Courier New Bold.ttf"
RED, EMBER, LIGHT, GREY = (226, 59, 46), (255, 122, 42), (232, 222, 208), (150, 138, 128)

def text_layer(size, text, font, pt, col, y, glow=None, pill=None):
    im = Image.new("RGBA", size, (0, 0, 0, 0)); d = ImageDraw.Draw(im); f = ImageFont.truetype(font, pt)
    x0, y0, x1, y1 = d.textbbox((0, 0), text, font=f); w, h = x1 - x0, y1 - y0
    x = (size[0] - w) / 2 - x0
    if pill:
        px, py = 44, 22
        d.rounded_rectangle(((size[0] - w) / 2 - px, y - py, (size[0] + w) / 2 + px, y + h + py), radius=14, fill=pill)
    if glow:
        g = Image.new("RGBA", size, (0, 0, 0, 0)); ImageDraw.Draw(g).text((x, y - y0), text, font=f, fill=glow)
        im = Image.alpha_composite(im, g.filter(ImageFilter.GaussianBlur(18)))
        d = ImageDraw.Draw(im)
    d.text((x, y - y0), text, font=f, fill=col + (255,))
    return im

def bg(still, size, dark=0.32):
    im = Image.open(still).convert("RGB")
    sw, sh = im.size; tw, th = size
    s = max(tw / sw, th / sh); im = im.resize((int(sw * s) + 1, int(sh * s) + 1))
    l, t = (im.width - tw) // 2, (im.height - th) // 2
    im = im.crop((l, t, l + tw, t + th)).filter(ImageFilter.GaussianBlur(9))
    return Image.eval(im, lambda v: int(v * dark)).convert("RGBA")

def card(name, size, still, layers):
    bg(os.path.join(C, still), size).save(os.path.join(C, f"{name}_bg.png"))
    for i, args in enumerate(layers):
        text_layer(size, *args).save(os.path.join(C, f"{name}_{i}.png"))

W, H = 1920, 1080
card("intro", (W, H), "still_a.png", [
    ("HELLDROP", BLACK, 210, RED, 330, (226, 59, 46, 150)),
    ("R E V E R S E    D O O M", MONO, 50, LIGHT, 600),
    ("you are not the hero", MONO, 34, GREY, 700),
])
card("end", (W, H), "still_b.png", [
    ("HELLDROP", BLACK, 190, RED, 250, (226, 59, 46, 150)),
    ("Kill the hero.", MONO, 50, LIGHT, 500),
    ("COMING SOON TO THE APP STORE", BLACK, 46, (255, 255, 255), 640, None, (196, 32, 24, 255)),
    ("play free now at helldrop.ximg.app", MONO, 34, GREY, 800),
])
VW, VH = 1080, 1920
card("endv", (VW, VH), "still_b.png", [
    ("HELLDROP", BLACK, 150, RED, 600, (226, 59, 46, 150)),
    ("Kill the hero.", MONO, 50, LIGHT, 830),
    ("COMING SOON", BLACK, 64, (255, 255, 255), 1010, None, (196, 32, 24, 255)),
    ("TO THE APP STORE", BLACK, 64, (255, 255, 255), 1120),
    ("play free now at", MONO, 34, GREY, 1330),
    ("helldrop.ximg.app", MONO, 40, LIGHT, 1385),
])

# Short frame: gameplay sits in a 1080x1350 window at y=285; title band above, captions below.
fr = bg(os.path.join(C, "still_a.png"), (VW, VH), 0.18)
for lay in (text_layer((VW, VH), "HELLDROP", BLACK, 120, RED, 70, (226, 59, 46, 140)),
            text_layer((VW, VH), "YOU ARE THE DEMONS", MONO, 40, LIGHT, 215)):
    fr = Image.alpha_composite(fr, lay)
d = ImageDraw.Draw(fr); d.rectangle((0, 283, VW, 285 + 1350 + 2), fill=(226, 59, 46, 255))
fr.save(os.path.join(C, "short_frame.png"))

def caption(seg, text):
    im = Image.new("RGBA", (VW, VH), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    f = ImageFont.truetype(BLACK, 58); words = text.split(); lines = [""]
    for w in words:
        t = (lines[-1] + " " + w).strip()
        if d.textbbox((0, 0), t, font=f)[2] > VW - 120: lines.append(w)
        else: lines[-1] = t
    y = 1665
    for ln in lines:
        x0, y0, x1, y1 = d.textbbox((0, 0), ln, font=f); x = (VW - (x1 - x0)) / 2 - x0
        for dx, dy in ((-3, 0), (3, 0), (0, -3), (0, 3), (3, 3)):
            d.text((x + dx, y - y0 + dy), ln, font=f, fill=(0, 0, 0, 255))
        d.text((x, y - y0), ln, font=f, fill=(255, 240, 225, 255)); y += 74
    im.save(os.path.join(C, f"cap_{seg}.png"))

for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "cut-short.txt")):
    if line.startswith("#") or not line.strip(): continue
    p = [x.strip() for x in line.split("|")]
    if len(p) > 6 and p[6]: caption(p[0], p[6])
print("cards ok")
