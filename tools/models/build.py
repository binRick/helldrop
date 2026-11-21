"""Build Helldrop model GLBs.

    Blender -b --factory-startup -P tools/models/build.py -- <out_dir> [name ...]

Writes <out_dir>/<name>.glb, <out_dir>/<name>.json (meta for the manifest) and
<out_dir>/tex/<name>.png (the baked atlas, also embedded in the GLB).
"""
import os
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hdlib  # noqa: E402
import props  # noqa: E402
import roster  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
OUT = os.path.abspath(argv[0] if argv else "build/models_out")
ALL = dict(roster.BUILDERS)
ALL.update(props.BUILDERS)
names = argv[1:] or list(ALL)
os.makedirs(os.path.join(OUT, "tex"), exist_ok=True)
fails = []
for name in names:
    t0 = time.time()
    try:
        hdlib.reset()
        m = ALL[name](name, os.path.join(OUT, "tex", name + ".png"))
        m.export(os.path.join(OUT, name + ".glb"))
        m.write_meta(os.path.join(OUT, name + ".json"))
        print("BUILT %-14s tris=%6d bones=%2d %.1fs" % (name, m.tri_count(), len(m.bones), time.time() - t0), flush=True)
    except Exception:
        traceback.print_exc()
        fails.append(name)
        print("FAILED", name, flush=True)
print("DONE fails=%s" % fails, flush=True)
