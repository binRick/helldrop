"""Render preview PNGs of exported GLBs (EEVEE, 384x384, isometric-style 3/4 view from above).

    Blender -b --factory-startup -P tools/models/preview.py -- <glb_dir> <out_dir> [name ...]

For each model writes <out>/<name>.png (idle frame 0 / static) and, for rigged models,
<out>/<name>_death.png (last frame of "death": the corpse) and <out>/<name>_clips.png frames
(<name>_<clip>.png at ~60% of each one-shot clip).  Re-importing the GLB checks the export too.
"""
import math
import os
import sys

import bpy
import numpy as np
from mathutils import Vector

argv = sys.argv[sys.argv.index("--") + 1:]
GLB, OUT = os.path.abspath(argv[0]), os.path.abspath(argv[1])
names = argv[2:] or sorted(f[:-4] for f in os.listdir(GLB) if f.endswith(".glb"))
os.makedirs(OUT, exist_ok=True)
RES = 384


def setup():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "BLENDER_EEVEE"
    sc.render.resolution_x = sc.render.resolution_y = RES
    sc.render.film_transparent = False
    try:
        sc.view_settings.view_transform = "Standard"
    except Exception:
        pass
    w = bpy.data.worlds.new("w")
    sc.world = w
    bg = w.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.32, 0.34, 0.38, 1)
    bg.inputs[1].default_value = 1.0
    for rot, e in (((40, 0, -35), 3.4), ((55, 0, 150), 1.2), ((-10, 0, 200), 0.5)):
        L = bpy.data.lights.new("sun", "SUN")
        L.energy = e
        L.angle = math.radians(8)
        o = bpy.data.objects.new("sun", L)
        o.rotation_euler = [math.radians(a) for a in rot]
        sc.collection.objects.link(o)
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    sc.collection.objects.link(cam)
    sc.camera = cam
    # ground disc
    bpy.ops.mesh.primitive_circle_add(vertices=48, radius=1.0, fill_type="NGON")
    g = bpy.context.active_object
    g.name = "ground"
    gm = bpy.data.materials.new("g")
    bs = next(n for n in gm.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    bs.inputs["Base Color"].default_value = (0.11, 0.11, 0.12, 1)
    bs.inputs["Roughness"].default_value = 0.9
    g.data.materials.append(gm)
    return sc, cam, g


def bounds(objs):
    dg = bpy.context.evaluated_depsgraph_get()
    pts = []
    for o in objs:
        ev = o.evaluated_get(dg)
        me = ev.to_mesh()
        co = np.empty(len(me.vertices) * 3, np.float32)
        me.vertices.foreach_get("co", co)
        M = np.array(o.matrix_world)
        co = co.reshape(-1, 3) @ M[:3, :3].T + M[:3, 3]
        pts.append(co)
        ev.to_mesh_clear()
    P = np.concatenate(pts)
    return P.min(0), P.max(0)


def frame_cam(cam, ground, mn, mx, az=35.0, el=32.0):
    c = (mn + mx) / 2
    ext = mx - mn
    size = max(ext[2] * 1.05, math.hypot(ext[0], ext[1]) * 0.95, 0.4)
    a, e = math.radians(az), math.radians(el)
    d = Vector((math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e)))
    tgt = Vector((c[0], c[1], c[2]))
    cam.location = tgt + d * 20
    cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
    cam.data.ortho_scale = size * 1.3
    cam.data.clip_end = 100
    ground.location = (c[0], c[1], min(0.0, mn[2]) - 0.002)
    ground.scale = (size * 1.2,) * 3


def render(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


for name in names:
    sc, cam, ground = setup()
    before = set(sc.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(GLB, name + ".glb"))
    for o in [o for o in sc.objects if o not in before and o.type == "MESH" and not o.visible_get()]:
        bpy.data.objects.remove(o, do_unlink=True)          # importer's bone-shape helpers
    meshes = [o for o in sc.objects if o.type == "MESH" and o not in before]
    arm = next((o for o in sc.objects if o.type == "ARMATURE"), None)
    acts = {}
    if arm:
        for a in bpy.data.actions:
            acts[a.name.split("_")[0].split(".")[0]] = a
        ad = arm.animation_data or arm.animation_data_create()
        for tr in list(ad.nla_tracks):
            ad.nla_tracks.remove(tr)

    def use(clip, frac):
        act = acts[clip]
        ad = arm.animation_data
        ad.action = act
        if hasattr(ad, "action_slot") and act.slots:
            ad.action_slot = act.slots[0]
        f0, f1 = act.frame_range
        sc.frame_set(int(round(f0 + (f1 - f0) * frac)))
    if arm and "idle" in acts:
        use("idle", 0.0)
    mn, mx = bounds(meshes)
    frame_cam(cam, ground, mn, mx)
    render(os.path.join(OUT, name + ".png"))
    if arm:
        for clip, frac in (("walk", 0.25), ("attack", 0.6), ("pain", 0.3)):
            if clip in acts:
                use(clip, frac)
                render(os.path.join(OUT, "%s_%s.png" % (name, clip)))
        if "death" in acts:
            use("death", 1.0)
            dmn, dmx = bounds(meshes)
            frame_cam(cam, ground, np.minimum(mn, dmn), np.maximum(mx, dmx))
            render(os.path.join(OUT, name + "_death.png"))
            print("DEATHBOX %s %s %s" % (name, np.round(dmn, 3).tolist(), np.round(dmx, 3).tolist()), flush=True)
    print("PREVIEW", name, flush=True)
