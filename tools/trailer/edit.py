#!/usr/bin/env python3
"""Assemble the trailer video:  edit.py <build/trailer> <cut.txt> <long|short> <mix.wav> <out.mp4>
long : 1920x1080 30 fps; take segments cut straight in, cards = blurred still + text layers fading in.
short: 1080x1920; each take's centre 4:5 crop sits in short_frame.png's window (y=285, 1080x1350),
       with that segment's caption (cards/cap_<seg>.png); cards use the vertical endv layers.
Runs ffmpeg only (the mini host does the encoding)."""
import glob, os, subprocess, sys

B, CUT, MODE, MIX, OUT = sys.argv[1:6]
W = os.path.join(B, "work-" + MODE); os.makedirs(W, exist_ok=True)
C = os.path.join(B, "cards"); FPS = 30
VW, VH = (1920, 1080) if MODE == "long" else (1080, 1920)

def ff(*a):
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", *a], check=True)

def card(name, dur, out, fade_out):
    layers = sorted(glob.glob(os.path.join(C, f"{name}_[0-9].png")))
    args = ["-loop", "1", "-t", str(dur), "-i", os.path.join(C, f"{name}_bg.png")]
    for l in layers: args += ["-loop", "1", "-t", str(dur), "-i", l]
    fc = f"[0]format=rgba,fade=t=in:st=0:d=0.6[b0];"
    for i in range(len(layers)):
        st = 0.25 + 0.55 * i
        fc += f"[{i+1}]format=rgba,fade=t=in:st={st:.2f}:d=0.7:alpha=1[l{i}];"
    prev = "b0"
    for i in range(len(layers)):
        fc += f"[{prev}][l{i}]overlay[o{i}];"; prev = f"o{i}"
    fo = f",fade=t=out:st={dur - 0.6:.2f}:d=0.6" if fade_out else ""
    fc += f"[{prev}]fps={FPS},scale={VW}:{VH},format=yuv420p{fo}[v]"
    ff(*args, "-filter_complex", fc, "-map", "[v]", "-t", str(dur), "-r", str(FPS),
       "-c:v", "libx264", "-crf", "16", "-preset", "medium", out)

segs = []
for line in open(CUT):
    if line.startswith("#") or not line.strip(): continue
    p = [x.strip() for x in line.split("|")]
    segs.append(p)

parts = []
for i, p in enumerate(segs):
    name, take, tin, dur = p[0], p[1], float(p[2]), float(p[3])
    out = os.path.join(W, f"seg{i:02d}.mp4")
    if take.startswith("card:"):
        card(take[5:], dur, out, fade_out=(i == 0))
    elif MODE == "long":
        fi = ",fade=t=in:st=0:d=0.5" if i > 0 and segs[i - 1][1].startswith("card:") else ""
        ff("-ss", str(tin), "-t", str(dur), "-i", os.path.join(B, "takes", take, "take.mp4"),
           "-vf", f"fps={FPS},scale={VW}:{VH}:flags=lanczos,format=yuv420p{fi}", "-an", "-t", str(dur),
           "-c:v", "libx264", "-crf", "16", "-preset", "medium", out)
    else:
        cap = os.path.join(C, f"cap_{name}.png")
        args = ["-loop", "1", "-t", str(dur), "-i", os.path.join(C, "short_frame.png"),
                "-ss", str(tin), "-t", str(dur), "-i", os.path.join(B, "takes", take, "take.mp4")]
        fc = (f"[1]fps={FPS},crop=ih*4/5:ih:(iw-ih*4/5)/2:0,scale=1080:1350:flags=lanczos[g];"
              f"[0][g]overlay=0:285[a]")
        if os.path.exists(cap):
            args += ["-loop", "1", "-t", str(dur), "-i", cap]
            fc += f";[2]format=rgba,fade=t=in:st=0:d=0.2:alpha=1[c];[a][c]overlay[a2]"
            last = "a2"
        else:
            last = "a"
        fi = ",fade=t=in:st=0:d=0.35" if i == 0 else ""
        fc += f";[{last}]fps={FPS},format=yuv420p{fi}[v]"
        ff(*args, "-filter_complex", fc, "-map", "[v]", "-t", str(dur), "-r", str(FPS),
           "-c:v", "libx264", "-crf", "17", "-preset", "medium", out)
    parts.append(out)

lst = os.path.join(W, "list.txt")
open(lst, "w").write("".join(f"file '{os.path.basename(x)}'\n" for x in parts))
body = os.path.join(W, "body.mp4")
ff("-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", body)
ff("-i", body, "-i", MIX, "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
   "-ar", "48000", "-shortest", "-movflags", "+faststart", OUT)
print("built", OUT)
