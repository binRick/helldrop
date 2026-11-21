"""Merge the Blender build metadata with the raylib check results into assets/models/manifest.json
and fail loudly on anything the game would trip over.

    python3 tools/models/make_manifest.py <build_out_dir> <check.ndjson> <manifest.json>

Checks (raylib 6.2's own loader, see check_models.c): one mesh with a real PNG texture, the five
clips with the exact names, every clip valid for the skeleton, <= 64 bones, and the death
clip's LAST frame lying down (walkers: lowest point on the ground and posed height under 55% of
standing or lower than the corpse is long (wide brutes lie on their side); flyers: resting on the ground, no lower than it).
"""
import json
import os
import sys

CLIPS = ["idle", "walk", "attack", "pain", "death"]


def main():
    out_dir, check_path, manifest_path = sys.argv[1:4]
    checks = {}
    for line in open(check_path):
        line = line.strip()
        if line.startswith("{"):
            c = json.loads(line)
            checks[os.path.splitext(os.path.basename(c["file"]))[0]] = c
    manifest, problems = {}, []
    names = sorted(f[:-5] for f in os.listdir(out_dir) if f.endswith(".json"))
    print("%-14s %6s %5s %6s %6s %8s  %s" % ("model", "tris", "bones", "height", "radius", "tex", "clips (frames @60fps)"))
    for n in names:
        meta = json.load(open(os.path.join(out_dir, n + ".json")))
        c = checks.get(n)
        if c is None:
            problems.append("%s: not checked by raylib" % n)
            continue
        entry = dict(kind=meta["kind"], height=meta["height"], radius=meta["radius"], tris=c["tris"],
                     bones=c["bones"], doom_height=meta["doom_height"], doom_radius=meta["doom_radius"],
                     doom_scale=meta["doom_scale"], texture=c["tex"], clips={})
        if c["meshes"] != 1:
            problems.append("%s: %d meshes" % (n, c["meshes"]))
        if c["tex"][0] < 64:
            problems.append("%s: no albedo texture loaded (%s)" % (n, c["tex"]))
        if meta["kind"] != "prop":
            if c["bones"] > 64:
                problems.append("%s: %d bones" % (n, c["bones"]))
            for clip in CLIPS:
                a = c["anims"].get(clip)
                if a is None:
                    problems.append("%s: missing clip %s" % (n, clip))
                    continue
                if not a["valid"]:
                    problems.append("%s: clip %s invalid for the skeleton" % (n, clip))
                entry["clips"][clip] = {"frames": a["frames"], "fps": 60}
            extra = sorted(set(c["anims"]) - set(CLIPS))
            if extra:
                problems.append("%s: unexpected clips %s" % (n, extra))
            d = c["anims"].get("death")
            if d:
                rest_top, top, low = c["rest"][4], d["last"][4], d["last"][1]
                entry["corpse_height"] = round(top - low, 4)
                if abs(low) > 0.02:
                    problems.append("%s: corpse not on the ground (min y %.3f)" % (n, low))
                span = max(d["last"][3] - d["last"][0], d["last"][5] - d["last"][2])
                if meta["kind"] == "walker" and top > 0.55 * rest_top and top > span:
                    problems.append("%s: corpse not lying (%.2f tall, %.2f long, stood %.2f)" % (n, top, span, rest_top))
        manifest[n] = entry
        print("%-14s %6d %5d %6.3f %6.3f %8s  %s" % (n, c["tris"], c["bones"], meta["height"], meta["radius"],
                                                   "%dx%d" % tuple(c["tex"]),
                                                   " ".join("%s:%d" % (k, v["frames"]) for k, v in entry["clips"].items())))
    doc = {"_about": {"units": "model units; at authoring scale 1 Doom map unit = 1/32 model unit. "
                               "Multiply by doom_scale to stand exactly doom_height map units tall.",
                      "facing": "+Z (glTF), feet / hover ground point at the origin, Y up",
                      "clips": "raylib resamples at 60 fps; frames = keyframeCount from LoadModelAnimations",
                      "corpse": "hold the LAST frame of 'death'",
                      "rebuild": "tools/models/build_all.sh"}}
    doc.update(manifest)
    with open(manifest_path, "w") as f:
        json.dump(doc, f, indent=1)
    print("wrote", manifest_path, len(manifest), "models")
    if problems:
        print("PROBLEMS:\n  " + "\n  ".join(problems))
        sys.exit(1)
    print("all checks passed")


if __name__ == "__main__":
    main()
