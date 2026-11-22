#!/usr/bin/env python3
"""The trailer soundtrack, in numpy:  mix.py <build/trailer> <cut.txt> <out.wav>
- game sound: each take's sound.txt ("seconds lump gain") replayed from snd/<lump>.wav, cut to the segment
- a synthesized low drone under everything (continuous across cuts, so no seams)
- narration: ElevenLabs clips trimmed of stray tails, edge-faded, placed per the cut; game ducks under it
- one fixed gain to -14 LUFS (measured by ffmpeg), true peak kept under -1.5 dBTP
Every edge is faded (mixing-glitch-free-voiceovers): clips 12 ms in / 60 ms out, segments 40 ms, video end 1.5 s."""
import json, os, subprocess, sys
import numpy as np, soundfile as sf

SR = 48000
B, CUT, OUT = sys.argv[1], sys.argv[2], sys.argv[3]

def ramp(n):
    return (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, max(n, 1)))).astype("float32")[:, None]

def fade(a, fin, fout):
    a = a.copy(); n = min(int(fin * SR), len(a) // 2)
    if n: a[:n] *= ramp(n)
    n = min(int(fout * SR), len(a) // 2)
    if n: a[-n:] *= ramp(n)[::-1]
    return a

def load(path):
    a, sr = sf.read(path, dtype="float32", always_2d=True)
    if a.shape[1] == 1: a = np.repeat(a, 2, axis=1)
    if sr != SR:
        t = np.arange(int(len(a) * SR / sr)) * sr / SR
        a = np.stack([np.interp(t, np.arange(len(a)), a[:, c]) for c in range(2)], axis=1).astype("float32")
    return a

def decode(path):   # mp3 -> float via ffmpeg
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "2", "-ar", str(SR), "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype="float32").reshape(-1, 2).copy()

def vo_trim(a, name=""):
    """Cut ElevenLabs' end-of-clip blips. TTS clips often end: speech, 60 ms-1 s of digital
    silence, then a short burst (often loud, often still sounding when the file stops). Working
    back from the end, drop every burst <= 200 ms that follows >= 60 ms of near-silence, then end
    0.12 s after the real speech. Gaps inside speech are shorter or quieter, so words survive."""
    lvl = np.abs(a).max(axis=1); win = int(0.01 * SR)
    db = np.array([20 * np.log10(max(float(np.sqrt(np.mean(lvl[i:i + win] ** 2))), 1e-7))
                   for i in range(0, len(lvl) - win + 1, win)])
    pk = db.max(); on = db > pk - 45
    runs, st = [], None
    for i, v in enumerate(list(on) + [False]):
        if v and st is None: st = i
        if not v and st is not None: runs.append([st, i]); st = None
    dropped = []
    while len(runs) > 1 and runs[-1][1] - runs[-1][0] <= 20 and runs[-1][0] - runs[-2][1] >= 6:
        dropped.append((runs[-1][0] / 100, runs[-1][1] / 100)); runs.pop()
    if dropped: print(f"  {name}: dropped end blip(s) at {dropped} s")
    s, t = runs[0][0] * win, runs[-1][1] * win
    if t >= len(a) - win: print(f"  {name}: speech runs to the file's end (cut mid-word?); faded")
    return fade(a[max(0, s - win): t + int(0.12 * SR)], 0.012, 0.06)

segs = []
for line in open(CUT):
    if line.startswith("#") or not line.strip(): continue
    p = [x.strip() for x in line.split("|")]
    segs.append(dict(name=p[0], take=p[1], tin=float(p[2]), dur=float(p[3]), vo=p[4], at=float(p[5])))
total = sum(s["dur"] for s in segs); N = int(total * SR) + 1
game = np.zeros((N, 2), "float32"); voice = np.zeros((N, 2), "float32"); marks = []
cache = {}
t0 = 0.0
for s in segs:
    a0, n = int(t0 * SR), int(s["dur"] * SR)
    marks.append(round(t0, 3))
    take = s["take"]
    if not take.startswith("card:") and os.path.exists(os.path.join(B, "takes", take, "sound.txt")):
        seg = np.zeros((n, 2), "float32")
        for row in open(os.path.join(B, "takes", take, "sound.txt")):
            ts, lump, g = row.split(); ts, g = float(ts), float(g)
            if ts > s["tin"] + s["dur"] or ts < s["tin"] - 3.0: continue
            key = lump
            if key not in cache:
                w = os.path.join(B, "takes", take, "snd", lump + ".wav")
                cache[key] = fade(load(w), 0.003, 0.02) if os.path.exists(w) else None
            clip = cache[key]
            if clip is None: continue
            j = int((ts - s["tin"]) * SR); k0 = max(0, -j); j = max(0, j)
            m = min(len(clip) - k0, n - j)
            if m > 0: seg[j:j + m] += clip[k0:k0 + m] * (0.45 * g)
        game[a0:a0 + n] += fade(seg, 0.04, 0.04)
    if s["vo"] != "-":
        v = vo_trim(decode(os.path.join(B, "vo", s["vo"] + ".mp3")), s["vo"])
        j = a0 + int(s["at"] * SR)
        if j + len(v) > N: sys.exit(f"narration {s['vo']} runs past the end")
        if np.abs(voice[j:j + len(v)]).max() > 0: sys.exit(f"narration {s['vo']} overlaps the previous line")
        voice[j:j + len(v)] += v * 1.0
    t0 += s["dur"]

# drone: two low tones a tritone apart with slow swells, plus soft filtered noise
t = np.arange(N) / SR
sw = 0.6 + 0.4 * np.sin(2 * np.pi * t / 7.3)
drone = (0.55 * np.sin(2 * np.pi * 41.2 * t) + 0.35 * np.sin(2 * np.pi * 58.3 * t + 1.1)
         + 0.18 * np.sin(2 * np.pi * 82.4 * t + 0.4)) * sw
rng = np.random.default_rng(7); noise = rng.standard_normal(N).astype("float32")
k = int(SR / 120); noise = np.convolve(noise, np.ones(k) / k, mode="same") * 6.0
bed = ((drone + 0.25 * noise) * 0.09).astype("float32")[:, None].repeat(2, axis=1)
bed = fade(bed, 1.5, 2.0)

# game sound peaks (several shots on one tic) would cap the loudness: a look-ahead
# peak limiter on the game track only, so the voice keeps its dynamics
def limit(a, thr=0.30, look=0.005, release=0.08):
    lvl = np.abs(a).max(axis=1); w = int(look * SR)
    from numpy.lib.stride_tricks import sliding_window_view
    pk = np.concatenate([sliding_window_view(np.pad(lvl, (w, w)), 2 * w + 1).max(axis=1)])[:len(lvl)]
    g = np.minimum(1.0, thr / np.maximum(pk, 1e-6))
    k = int(release * SR); g = np.convolve(g, np.ones(k) / k, mode="same")
    g = np.minimum(g, np.minimum(1.0, thr / np.maximum(pk, 1e-6)) * 1.0 + 0.0)
    return (a * g[:, None]).astype("float32")
game = limit(game)

# duck game + bed under the voice (smooth 150 ms envelope)
env = np.abs(voice).max(axis=1); k = int(0.15 * SR)
env = np.convolve((env > 0.01).astype("float32"), np.ones(k) / k, mode="same")
duck = (1.0 - 0.55 * np.clip(env, 0, 1))[:, None]
mix = (game + bed) * duck + voice * 1.35
mix = fade(mix, 0.0, 1.5)
sf.write(OUT + ".pre.wav", mix, SR, subtype="FLOAT")
meas = subprocess.run(["ffmpeg", "-v", "info", "-i", OUT + ".pre.wav", "-af", "loudnorm=I=-14:TP=-1.5:print_format=json",
                       "-f", "null", "-"], capture_output=True, text=True).stderr
js = json.loads(meas[meas.rfind("{"):meas.rfind("}") + 1])
gain = -14.0 - float(js["input_i"])
mix *= 10 ** (gain / 20)
mix = limit(mix, thr=0.80, look=0.003, release=0.05)   # brick wall at about -2 dBFS
sf.write(OUT, mix, SR, subtype="FLOAT")
chk = subprocess.run(["ffmpeg", "-v", "info", "-i", OUT, "-af", "loudnorm=I=-14:TP=-1.5:print_format=json",
                      "-f", "null", "-"], capture_output=True, text=True).stderr
cj = json.loads(chk[chk.rfind("{"):chk.rfind("}") + 1])
print(f"  final: {float(cj['input_i']):.1f} LUFS, true peak {float(cj['input_tp']):.1f} dBTP")
os.remove(OUT + ".pre.wav")
json.dump({"marks": marks, "total": total, "gain_db": gain, "input_lufs": float(js["input_i"])},
          open(OUT + ".json", "w"))
print(f"{OUT}: {total:.1f}s, gain {gain:+.1f} dB (from {float(js['input_i']):.1f} LUFS), marks {marks}")
