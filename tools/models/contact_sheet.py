"""Stitch preview PNGs into contact sheets (system python3 + Pillow; run locally).

    python3 tools/models/contact_sheet.py <prev_dir> <out_dir>

Writes contact_sheet.png (idle / static view of every model), death_sheet.png (last frame of
every death clip = the corpse), clips_sheet.png (walk/attack/pain/death per rigged model) and
copies the per-model PNGs.
"""
import os
import shutil
import sys

from PIL import Image, ImageDraw, ImageFont

ORDER = ["marine", "zombie", "shotgunner", "chaingunner", "imp", "pinky", "caco", "lostsoul", "revenant", "baron",
         "hellknight", "archvile", "mancubus", "painelemental", "barrel", "pk_medikit", "pk_stimpack", "pk_armor",
         "pk_shotgun", "pk_chaingun", "pk_rlaunch", "pk_clip", "pk_clipbox", "pk_shells", "pk_shellbox", "pk_rocket",
         "pk_rocketbox"]


def font(sz):
    for p in ("/System/Library/Fonts/Supplemental/PTSans.ttc", "/System/Library/Fonts/Helvetica.ttc"):
        try:
            return ImageFont.truetype(p, sz)
        except Exception:
            pass
    return ImageFont.load_default()


def sheet(cells, cols, out, title):
    if not cells:
        return
    w, h = Image.open(cells[0][1]).size
    rows = (len(cells) + cols - 1) // cols
    top = 44
    im = Image.new("RGB", (cols * w, rows * (h + 26) + top), (24, 25, 28))
    d = ImageDraw.Draw(im)
    d.text((12, 8), title, fill=(235, 235, 235), font=font(26))
    f = font(20)
    for i, (label, path) in enumerate(cells):
        x, y = (i % cols) * w, top + (i // cols) * (h + 26)
        im.paste(Image.open(path).convert("RGB"), (x, y + 26))
        d.text((x + 8, y + 2), label, fill=(230, 210, 160), font=f)
    im.save(out)
    print("wrote", out, im.size)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    have = [n for n in ORDER if os.path.exists(os.path.join(src, n + ".png"))]
    have += sorted(f[:-4] for f in os.listdir(src) if f.endswith(".png") and "_" not in f.replace("pk_", "")
                   and f[:-4] not in have)
    for f in os.listdir(src):
        if f.endswith(".png"):
            shutil.copy(os.path.join(src, f), os.path.join(out, f))
    sheet([(n, os.path.join(src, n + ".png")) for n in have], 6, os.path.join(out, "contact_sheet.png"),
          "Helldrop model roster - idle frame 0 / static (EEVEE, 3/4 iso view)")
    dead = [n for n in have if os.path.exists(os.path.join(src, n + "_death.png"))]
    sheet([(n + " corpse", os.path.join(src, n + "_death.png")) for n in dead], 5, os.path.join(out, "death_sheet.png"),
          "Death clip LAST frame (held as the corpse)")
    cells = []
    for n in dead:
        for c in ("walk", "attack", "pain", "death"):
            p = os.path.join(src, "%s_%s.png" % (n, c))
            if os.path.exists(p):
                cells.append(("%s %s" % (n, c), p))
    sheet(cells, 4, os.path.join(out, "clips_sheet.png"), "Clip frames: walk 25%, attack 60% (strike), pain 30%, death end")


if __name__ == "__main__":
    main()
