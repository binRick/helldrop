"""hdlib -- shared Blender 5.2 helpers for the Helldrop model roster.

Authoring space ("char space"): x = the character's RIGHT, y = FORWARD, z = UP, in Doom map
units.  cv() maps it to Blender (character faces -Y, export_yup=True turns that into glTF +Z)
and scales by U (1 Doom unit = 1/32 model unit, so a 56-unit marine is 1.75 model units).

Pipeline per model:
  parts  -- bmesh primitives (box/cyl/ellip/tube) rigidly bound to one bone, or Skin-modifier
            bodies (edge skeleton + radii -> Skin -> Subdivision -> Displace) auto-weighted by
            normalised distance to the skin edges (each edge is tagged with its bone).
  paints -- procedural Cycles node materials (noise variation, pattern, AO grime, bevel-edge
            wear, glow) baked to ONE PNG texture on a Smart-UV atlas, then swapped for one
            plain textured Principled material (raylib reads the base-colour texture only).
  rig    -- armature built from the same joint list; clips keyed procedurally on a 60 fps
            timeline, one NLA track per clip, exported as named glTF animations.
"""
import json
import math
import random

import bmesh
import bpy
import numpy as np
from mathutils import Euler, Matrix, Quaternion, Vector

U = 1.0 / 32.0
CONV = Matrix(((-U, 0, 0, 0), (0, -U, 0, 0), (0, 0, U, 0), (0, 0, 0, 1)))


def cv(p):
    return Vector((-p[0] * U, -p[1] * U, p[2] * U))


def cax(a):
    return Vector((-a[0], -a[1], a[2]))


def lerp(a, b, t):
    return a + (b - a) * t


def smooth(t):
    t = min(1.0, max(0.0, t))
    return t * t * (3 - 2 * t)


def seg(t, a, b):
    """0 before a, 1 after b, smoothstep between."""
    return smooth((t - a) / (b - a)) if b > a else float(t >= a)


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.fps = 60


def link(ob):
    bpy.context.scene.collection.objects.link(ob)
    return ob


# =================================================================== rotations (char space)
def Q(pitch=0.0, yaw=0.0, roll=0.0):
    """pitch>0: an upward bone's top tilts FORWARD (a hanging limb swings BACK).
    yaw>0: turns to the character's left.  roll>0: an upward bone's top tilts to the RIGHT."""
    q = Quaternion(cax((0, 0, 1)), math.radians(yaw))
    q = q @ Quaternion(cax((1, 0, 0)), math.radians(-pitch))
    q = q @ Quaternion(cax((0, 1, 0)), math.radians(roll))
    return q


# =================================================================== materials (paints)
def _n(nt, kind, **props):
    n = nt.nodes.new(kind)
    for k, v in props.items():
        setattr(n, k, v)
    return n


def _in(node, key):
    for s in node.inputs:
        if s.identifier == key:
            return s
    return node.inputs[key]


def _out(node, key):
    for s in node.outputs:
        if s.identifier == key:
            return s
    return node.outputs[key]


def _mix(nt, a, b, fac, blend="MIX"):
    m = _n(nt, "ShaderNodeMix", data_type="RGBA", blend_type=blend)
    m.clamp_result = True
    for sock, src in (("A_Color", a), ("B_Color", b), ("Factor_Float", fac)):
        if isinstance(src, (tuple, list)):
            _in(m, sock).default_value = tuple(src) + ((1.0,) if len(src) == 3 else ())
        elif isinstance(src, (int, float)):
            _in(m, sock).default_value = src
        else:
            nt.links.new(src, _in(m, sock))
    return _out(m, "Result_Color")


def _maprange(nt, src, a, b, c=0.0, d=1.0):
    m = _n(nt, "ShaderNodeMapRange", clamp=True)
    nt.links.new(src, m.inputs[0])
    m.inputs[1].default_value, m.inputs[2].default_value = a, b
    m.inputs[3].default_value, m.inputs[4].default_value = c, d
    return m.outputs[0]


def _math(nt, op, a, b=None):
    m = _n(nt, "ShaderNodeMath", operation=op)
    for i, src in enumerate((a, b)):
        if src is None:
            continue
        if isinstance(src, (int, float)):
            m.inputs[i].default_value = src
        else:
            nt.links.new(src, m.inputs[i])
    return m.outputs[0]


def _noise(nt, coord, scale, detail=6.0, rough=0.55):
    n = _n(nt, "ShaderNodeTexNoise")
    nt.links.new(coord, n.inputs["Vector"])
    n.inputs["Scale"].default_value = scale
    n.inputs["Detail"].default_value = detail
    n.inputs["Roughness"].default_value = rough
    return n.outputs["Fac"]


def _srgb(c):
    """Paint colours are authored as sRGB 0..1; Emission wants linear."""
    def f(x):
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
    return tuple(f(x) for x in c[:3])


def _scale_col(nt, col, fac):
    sc = _n(nt, "ShaderNodeVectorMath", operation="SCALE")
    if isinstance(col, tuple):
        sc.inputs[0].default_value = col
    else:
        nt.links.new(col, sc.inputs[0])
    if isinstance(fac, (int, float)):
        sc.inputs["Scale"].default_value = fac
    else:
        nt.links.new(fac, sc.inputs["Scale"])
    return sc.outputs["Vector"]


def make_paint(key, color, color2=None, pattern=None, pscale=4.0, pamount=1.0, var=0.18, vscale=6.0,
               wear=0.35, wear_color=None, grime=0.55, grime_color=(0.08, 0.06, 0.05), ao_dist=4.0,
               glow=False, edge_radius=0.6, zband=None, spots=None, seams=None, pointy=0.0):
    """Procedural paint -> Emission (baked to the atlas).  Scales are in Doom units."""
    m = bpy.data.materials.new("P_" + key)
    m.use_fake_user = True
    nt = m.node_tree
    nt.nodes.clear()
    out = _n(nt, "ShaderNodeOutputMaterial")
    emi = _n(nt, "ShaderNodeEmission")
    emi.inputs["Strength"].default_value = 1.0
    nt.links.new(emi.outputs[0], out.inputs["Surface"])
    tc = _n(nt, "ShaderNodeTexCoord")
    co = _n(nt, "ShaderNodeVectorMath", operation="SCALE")
    nt.links.new(tc.outputs["Object"], co.inputs[0])
    co.inputs["Scale"].default_value = 1.0 / U        # coordinates back in Doom units
    co = co.outputs["Vector"]
    vn = _noise(nt, co, 1.0 / vscale, detail=8, rough=0.6)
    vfac = _maprange(nt, vn, 0.3, 0.7, 1.0 - var, 1.0 + var)
    col = _scale_col(nt, _srgb(color), vfac)
    if color2 is not None and pattern:
        if pattern == "camo":
            mask = _maprange(nt, _noise(nt, co, 1.0 / pscale, detail=3, rough=0.4), 0.47, 0.53)
        elif pattern == "mottle":
            mask = _maprange(nt, _noise(nt, co, 1.0 / pscale, detail=6, rough=0.6), 0.42, 0.68)
        elif pattern in ("veins", "plates", "cracks"):
            vo = _n(nt, "ShaderNodeTexVoronoi", feature="DISTANCE_TO_EDGE")
            nn = _n(nt, "ShaderNodeTexNoise")
            nt.links.new(co, nn.inputs["Vector"])
            nn.inputs["Scale"].default_value = 1.0 / pscale
            warp = _scale_col(nt, nn.outputs["Color"], pscale * (0.6 if pattern == "veins" else 0.12))
            add = _n(nt, "ShaderNodeVectorMath", operation="ADD")
            nt.links.new(co, add.inputs[0])
            nt.links.new(warp, add.inputs[1])
            nt.links.new(add.outputs["Vector"], vo.inputs["Vector"])
            vo.inputs["Scale"].default_value = 1.0 / pscale
            w = {"veins": 0.06, "plates": 0.04, "cracks": 0.03}[pattern]
            mask = _maprange(nt, vo.outputs["Distance"], w, 0.0)
        elif pattern == "stripes":
            wv = _n(nt, "ShaderNodeTexWave")
            nt.links.new(co, wv.inputs["Vector"])
            wv.inputs["Scale"].default_value = 1.0 / pscale
            wv.inputs["Distortion"].default_value = 2.0
            mask = _maprange(nt, wv.outputs["Fac"], 0.45, 0.55)
        elif pattern == "spots":
            vo = _n(nt, "ShaderNodeTexVoronoi")
            nt.links.new(co, vo.inputs["Vector"])
            vo.inputs["Scale"].default_value = 1.0 / pscale
            mask = _maprange(nt, vo.outputs["Distance"], 0.32, 0.22)
        else:
            raise ValueError(pattern)
        if pamount != 1.0:
            mask = _math(nt, "MULTIPLY", mask, pamount)
        col = _mix(nt, col, _scale_col(nt, _srgb(color2), vfac), mask)
    if zband is not None:      # (z_from, z_to, colour): below z_from fully colour, fades out by z_to
        z0, z1, c3 = zband
        sep = _n(nt, "ShaderNodeSeparateXYZ")
        nt.links.new(co, sep.inputs[0])
        bn = _noise(nt, co, 0.25, detail=4)
        zz = _math(nt, "ADD", sep.outputs["Z"], _math(nt, "MULTIPLY", _math(nt, "SUBTRACT", bn, 0.5), 6.0))
        mask = _maprange(nt, zz, z1, z0)
        col = _mix(nt, col, _scale_col(nt, _srgb(c3), vfac), mask)
    if seams:                  # (cell size, darkness): panel lines / scale seams (voronoi cell edges)
        s_size, s_dark = seams
        vo = _n(nt, "ShaderNodeTexVoronoi", feature="DISTANCE_TO_EDGE")
        nt.links.new(co, vo.inputs["Vector"])
        vo.inputs["Scale"].default_value = 1.0 / s_size
        sm = _math(nt, "MULTIPLY", _maprange(nt, vo.outputs["Distance"], 0.045, 0.0), s_dark)
        col = _mix(nt, col, _scale_col(nt, col, 0.3), sm)
    if glow:
        hot = _maprange(nt, _noise(nt, co, 0.5, detail=3), 0.45, 0.75, 0.0, 0.5)
        col = _mix(nt, col, (1.0, 0.95, 0.8), hot)
        nt.links.new(col, emi.inputs["Color"])
        return m
    ao = _n(nt, "ShaderNodeAmbientOcclusion", samples=16)
    ao.inputs["Distance"].default_value = ao_dist * U
    aof = ao.outputs["AO"]
    if wear > 0:
        bev = _n(nt, "ShaderNodeBevel", samples=8)
        bev.inputs["Radius"].default_value = edge_radius * U
        geo = _n(nt, "ShaderNodeNewGeometry")
        dot = _n(nt, "ShaderNodeVectorMath", operation="DOT_PRODUCT")
        nt.links.new(bev.outputs["Normal"], dot.inputs[0])
        nt.links.new(geo.outputs["Normal"], dot.inputs[1])
        e = _math(nt, "SUBTRACT", 1.0, _out(dot, "Value"))
        edge = _maprange(nt, e, 0.01, 0.09)
        wm = _maprange(nt, _noise(nt, co, 0.6, detail=10, rough=0.7), 0.42, 0.6)
        wf = _math(nt, "MULTIPLY", _math(nt, "MULTIPLY", edge, wm), wear)
        wc = wear_color or tuple(min(1.0, x * 1.6 + 0.12) for x in color)
        col = _mix(nt, col, _srgb(wc), wf)
    if grime > 0:
        streak = _noise(nt, co, 0.3, detail=8, rough=0.7)
        g = _math(nt, "MULTIPLY", _maprange(nt, aof, 1.0, 0.35), grime)
        g = _math(nt, "MULTIPLY", g, _maprange(nt, streak, 0.3, 0.7, 0.6, 1.2))
        col = _mix(nt, col, _srgb(grime_color), g)
    if pointy:                  # organic definition: lighten convex ridges, darken creases (Cycles pointiness)
        geo2 = _n(nt, "ShaderNodeNewGeometry")
        pt = geo2.outputs["Pointiness"]
        cvx = _math(nt, "MULTIPLY", _maprange(nt, pt, 0.505, 0.55), pointy)
        ccv = _math(nt, "MULTIPLY", _maprange(nt, pt, 0.495, 0.45), pointy)
        col = _mix(nt, col, _scale_col(nt, col, 1.6), cvx)
        col = _mix(nt, col, _scale_col(nt, col, 0.4), ccv)
    if spots:                   # (colour, scale, amount): blood / scorch splats
        sc_col, s_scale, s_amt = spots
        sp = _maprange(nt, _noise(nt, co, 1.0 / s_scale, detail=4, rough=0.5), 0.64, 0.70, 0.0, s_amt)
        col = _mix(nt, col, _srgb(sc_col), sp)
    shade = _maprange(nt, aof, 0.0, 1.0, 0.45, 1.0)
    nt.links.new(_scale_col(nt, col, shade), emi.inputs["Color"])
    return m


# =================================================================== the model
class Model:
    def __init__(self, name, height, radius, tex=1024):
        self.name, self.height, self.radius, self.tex = name, height, radius, tex
        self.parts = []          # (object, bone or None=auto)
        self.paints = {}
        self.bones = []          # (name, head, tail, parent) char space
        self.segs = []           # skin edges for weighting: (p0, p1, r0, r1, bone) model space
        self.meta = {}
        self.rig = None

    # ---------------------------------------------------------- paints
    def paint(self, key, *a, **kw):
        self.paints[key] = make_paint(self.name + "_" + key, *a, **kw)

    def _finish(self, bm, paint, bone, name, sharp=35.0, flat=False):
        bmesh.ops.transform(bm, matrix=CONV, verts=bm.verts)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        me.polygons.foreach_set("use_smooth", [not flat] * len(me.polygons))
        if not flat and sharp:
            me.set_sharp_from_angle(angle=math.radians(sharp))
        ob = link(bpy.data.objects.new(name, me))
        ob.data.materials.append(self.paints[paint])
        self.parts.append((ob, bone))
        return ob

    @staticmethod
    def _place(bm, c, rot=(0, 0, 0), scale=None):
        M = Matrix.Translation(Vector(c)) @ Euler([math.radians(a) for a in rot], "XYZ").to_matrix().to_4x4()
        if scale is not None:
            M = M @ Matrix.Diagonal(Vector(tuple(scale) + (1.0,)))
        bmesh.ops.transform(bm, matrix=M, verts=bm.verts)

    # ---------------------------------------------------------- primitives (char space)
    def box(self, c, size, paint, bone, rot=(0, 0, 0), bevel=0.2, segs=2, taper=(1, 1), shear=(0, 0),
            name="box"):
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        for v in bm.verts:
            x, y, z = v.co
            if z > 0:
                x, y = x * taper[0] + shear[0], y * taper[1] + shear[1]
            v.co = Vector((x * size[0], y * size[1], z * size[2]))
        if bevel > 0:
            bmesh.ops.bevel(bm, geom=list(bm.edges), offset=bevel * min(size), segments=segs, profile=0.5,
                            affect="EDGES", clamp_overlap=True)
        self._place(bm, c, rot)
        return self._finish(bm, paint, bone, name)

    def cyl(self, p0, p1, r0, paint, bone, r1=None, segs=12, caps=True, name="cyl", sharp=35.0):
        p0, p1 = Vector(p0), Vector(p1)
        r1 = r0 if r1 is None else r1
        d = p1 - p0
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=caps, cap_tris=False, segments=segs, radius1=r0, radius2=r1,
                              depth=d.length)
        q = Vector((0, 0, 1)).rotation_difference(d.normalized())
        M = Matrix.Translation((p0 + p1) / 2) @ q.to_matrix().to_4x4()
        bmesh.ops.transform(bm, matrix=M, verts=bm.verts)
        return self._finish(bm, paint, bone, name, sharp=sharp)

    def ellip(self, c, radii, paint, bone, rot=(0, 0, 0), segs=(16, 10), name="ellip", noise=0.0):
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=segs[0], v_segments=segs[1], radius=1.0)
        if noise:
            rnd = random.Random(len(self.parts) * 7 + 3)
            for v in bm.verts:
                v.co *= 1.0 + noise * (rnd.random() - 0.5)
        self._place(bm, c, rot, radii)
        return self._finish(bm, paint, bone, name, sharp=0)

    def tube(self, pts, radii, paint, bone, segs=8, cap=True, name="tube", flat_ratio=1.0, up=(0, 0, 1)):
        """Tapered tube along a polyline; a radius of 0 at the end makes a point (horns, claws, teeth)."""
        pts = [Vector(p) for p in pts]
        n = len(pts)
        bm = bmesh.new()
        rings = []
        prev_side = None
        for i, p in enumerate(pts):
            t = (pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)]).normalized()
            side = Vector(up).cross(t)
            if side.length < 1e-4:
                side = Vector((1, 0, 0)).cross(t)
            side.normalize()
            if prev_side is not None and side.dot(prev_side) < 0:
                side = -side
            prev_side = side
            upv = t.cross(side).normalized()
            r = radii[i]
            if r <= 1e-6:
                rings.append([bm.verts.new(p)])
                continue
            rings.append([bm.verts.new(p + side * (math.cos(2 * math.pi * k / segs) * r) +
                                       upv * (math.sin(2 * math.pi * k / segs) * r * flat_ratio))
                          for k in range(segs)])
        for a, b in zip(rings, rings[1:]):
            if len(a) == 1 and len(b) == 1:
                continue
            if len(b) == 1:
                for k in range(segs):
                    bm.faces.new((a[k], a[(k + 1) % segs], b[0]))
            elif len(a) == 1:
                for k in range(segs):
                    bm.faces.new((b[(k + 1) % segs], b[k], a[0]))
            else:
                for k in range(segs):
                    bm.faces.new((a[k], a[(k + 1) % segs], b[(k + 1) % segs], b[k]))
        if cap:
            if len(rings[0]) > 1:
                bm.faces.new(list(reversed(rings[0])))
            if len(rings[-1]) > 1:
                bm.faces.new(rings[-1])
        return self._finish(bm, paint, bone, name, sharp=50)

    def horn(self, base, direction, length, r, paint, bone, curl=(0, 0, 0), segs=8, steps=6, name="horn"):
        """Curved tapering horn: the direction is bent by curl (deg per step about char x,y,z)."""
        d = Vector(direction).normalized()
        q = Euler([math.radians(a) for a in curl], "XYZ").to_quaternion()
        p = Vector(base)
        pts, rr = [p.copy()], [r]
        for i in range(steps):
            p = p + d * (length / steps)
            d = q @ d
            pts.append(p.copy())
            rr.append(r * (1 - (i + 1) / steps) ** 0.9)
        return self.tube(pts, rr, paint, bone, segs=segs, name=name)

    def sym(self, fn, *args, **kw):
        """Call a primitive twice, mirrored across x.  Bone names ending .R become .L.
        Sizes/radii (2nd positional arg of box/ellip, or size=/radii=) are not mirrored."""
        def mir(v):
            if isinstance(v, (tuple, list, Vector)) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v):
                return (-v[0], v[1], v[2])
            if isinstance(v, list) and v and isinstance(v[0], (tuple, list, Vector)):
                return [mir(x) for x in v]
            if isinstance(v, str) and v.endswith(".R"):
                return v[:-2] + ".L"
            return v
        kw_l = {k: mir(v) for k, v in kw.items() if k not in ("rot", "curl", "size", "radii")}
        for k in ("size", "radii"):
            if k in kw:
                kw_l[k] = kw[k]
        if "rot" in kw:
            rx, ry, rz = kw["rot"]
            kw_l["rot"] = (rx, -ry, -rz)
        if "curl" in kw:
            cx, cy, cz = kw["curl"]
            kw_l["curl"] = (cx, -cy, -cz)
        args_l = [mir(a) for a in args]
        if getattr(fn, "__name__", "") in ("box", "ellip") and len(args) > 1:
            args_l[1] = args[1]
        return fn(*args, **kw), fn(*args_l, **kw_l)

    # ---------------------------------------------------------- organic skin bodies
    def skin(self, chains, paint, subdiv=2, displace=0.6, dscale=5.0, name="body", branch=0.6):
        """chains: [ ([ (x,y,z,r), ...], bone or [bone per edge] ), ... ]; coincident points merge."""
        verts, rad, edges, index = [], [], [], {}

        def vid(p):
            k = (round(p[0], 2), round(p[1], 2), round(p[2], 2))
            if k not in index:
                index[k] = len(verts)
                verts.append(cv(p[:3]))
                r = p[3]
                rad.append((r, r) if isinstance(r, (int, float)) else tuple(r))
            return index[k]
        for pts, bones in chains:
            if isinstance(bones, str):
                bones = [bones] * (len(pts) - 1)
            assert len(bones) == len(pts) - 1, (name, len(pts), len(bones))
            ids = [vid(p) for p in pts]
            for i in range(len(ids) - 1):
                if ids[i] != ids[i + 1]:
                    edges.append((ids[i], ids[i + 1]))
                    r0, r1 = pts[i][3], pts[i + 1][3]
                    r0 = max(r0) if not isinstance(r0, (int, float)) else r0
                    r1 = max(r1) if not isinstance(r1, (int, float)) else r1
                    self.segs.append((cv(pts[i][:3]), cv(pts[i + 1][:3]), r0 * U, r1 * U, bones[i]))
        me = bpy.data.meshes.new(name + "_skel")
        me.from_pydata(verts, edges, [])
        ob = link(bpy.data.objects.new(name, me))
        sk = ob.modifiers.new("skin", "SKIN")
        sk.use_smooth_shade = True
        sk.branch_smoothing = branch
        sd = me.skin_vertices[0].data
        parent = list(range(len(verts)))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a
        for a, b in edges:
            parent[find(a)] = find(b)
        seen = set()
        for i, r in enumerate(rad):
            sd[i].radius = (r[0] * U, r[1] * U)
            root = find(i)
            sd[i].use_root = root not in seen
            seen.add(root)
        sub = ob.modifiers.new("sub", "SUBSURF")
        sub.levels = sub.render_levels = subdiv
        if displace:
            tex = bpy.data.textures.new(name + "_n", "CLOUDS")
            tex.noise_scale = dscale * U
            tex.noise_depth = 2
            dm = ob.modifiers.new("disp", "DISPLACE")
            dm.texture = tex
            dm.texture_coords = "LOCAL"
            dm.strength = displace * U
        dg = bpy.context.evaluated_depsgraph_get()
        me2 = bpy.data.meshes.new_from_object(ob.evaluated_get(dg))
        ob.modifiers.clear()
        ob.data = me2
        bpy.data.meshes.remove(me)
        me2.polygons.foreach_set("use_smooth", [True] * len(me2.polygons))
        ob.data.materials.append(self.paints[paint])
        self.parts.append((ob, None))
        return ob

    # ---------------------------------------------------------- skeleton
    def bone(self, name, head, tail, parent=None):
        self.bones.append((name, tuple(head), tuple(tail), parent))

    def bone_names(self):
        return [b[0] for b in self.bones]

    # ---------------------------------------------------------- assembly
    def _auto_weights(self, ob, bone_names):
        me = ob.data
        n = len(me.vertices)
        co = np.empty(n * 3, np.float32)
        me.vertices.foreach_get("co", co)
        co = co.reshape(-1, 3)
        P0 = np.array([s[0] for s in self.segs], np.float32)
        P1 = np.array([s[1] for s in self.segs], np.float32)
        R0 = np.array([s[2] for s in self.segs], np.float32)
        R1 = np.array([s[3] for s in self.segs], np.float32)
        sb = [s[4] for s in self.segs]
        d = P1 - P0
        L2 = np.maximum((d * d).sum(1), 1e-9)
        W = np.zeros((n, len(bone_names)), np.float32)
        bidx = {b: i for i, b in enumerate(bone_names)}
        for c0 in range(0, n, 4096):
            c = co[c0:c0 + 4096]
            t = np.clip(((c[:, None, :] - P0[None]) * d[None]).sum(2) / L2[None], 0, 1)
            proj = P0[None] + t[..., None] * d[None]
            dist = np.linalg.norm(c[:, None, :] - proj, axis=2)
            r = R0[None] + t * (R1 - R0)[None]
            q = dist / np.maximum(r, 1e-4)
            qmin = q.min(1, keepdims=True)
            w = np.exp(-(q - qmin) * 7.0)
            for j, b in enumerate(sb):
                W[c0:c0 + 4096, bidx[b]] += w[:, j]
        order = np.argsort(-W, axis=1)
        W2 = np.zeros_like(W)
        rows = np.arange(n)[:, None]
        W2[rows, order[:, :3]] = W[rows, order[:, :3]]
        W2 /= np.maximum(W2.sum(1, keepdims=True), 1e-9)
        W2[W2 < 0.03] = 0
        W2 /= np.maximum(W2.sum(1, keepdims=True), 1e-9)
        for j, b in enumerate(bone_names):
            idx = np.nonzero(W2[:, j] > 0)[0]
            if len(idx) == 0:
                continue
            vg = ob.vertex_groups.get(b) or ob.vertex_groups.new(name=b)
            for i in idx:
                vg.add([int(i)], float(W2[i, j]), "REPLACE")

    def assemble(self):
        """Weight every part, join into one mesh, build the armature, parent."""
        names = self.bone_names()
        for ob, bone in self.parts:
            if not names:
                continue
            if bone is None:
                self._auto_weights(ob, names)
            else:
                assert bone in names, (self.name, bone)
                vg = ob.vertex_groups.new(name=bone)
                vg.add(list(range(len(ob.data.vertices))), 1.0, "REPLACE")
        objs = [p[0] for p in self.parts]
        bpy.ops.object.select_all(action="DESELECT")
        for o in objs:
            o.select_set(True)
        bpy.context.view_layer.objects.active = objs[0]
        if len(objs) > 1:
            bpy.ops.object.join()
        body = objs[0]
        body.name = self.name
        body.data.name = self.name
        self.body = body
        if not self.bones:
            return body, None
        arm = bpy.data.armatures.new(self.name + "_rig")
        rig = link(bpy.data.objects.new(self.name + "_rig", arm))
        bpy.ops.object.select_all(action="DESELECT")
        rig.select_set(True)
        bpy.context.view_layer.objects.active = rig
        bpy.ops.object.mode_set(mode="EDIT")
        for name, h, t, par in self.bones:
            eb = arm.edit_bones.new(name)
            eb.head, eb.tail = cv(h), cv(t)
            eb.roll = 0.0
            eb.use_deform = True
            if par:
                eb.parent = arm.edit_bones[par]
                eb.use_connect = False
        bpy.ops.object.mode_set(mode="OBJECT")
        self.rig = rig
        body.parent = rig
        mod = body.modifiers.new("Armature", "ARMATURE")
        mod.object = rig
        for pb in rig.pose.bones:
            pb.rotation_mode = "QUATERNION"
        self.rest = {b.name: b.matrix_local.to_3x3() for b in arm.bones}
        return body, rig

    def tri_count(self):
        return sum(len(p.vertices) - 2 for p in self.body.data.polygons)

    def decimate_to(self, max_tris):
        t = self.tri_count()
        if t <= max_tris:
            return
        dm = self.body.modifiers.new("dec", "DECIMATE")
        dm.ratio = max_tris / t * 0.97
        bpy.context.view_layer.objects.active = self.body
        bpy.ops.object.modifier_move_to_index(modifier="dec", index=0)
        bpy.ops.object.modifier_apply(modifier="dec")

    # ---------------------------------------------------------- texture bake
    def bake(self, out_png, size=None, samples=24):
        size = size or self.tex
        ob = self.body
        sc = bpy.context.scene
        bpy.ops.object.select_all(action="DESELECT")
        ob.select_set(True)
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.003, area_weight=0.0,
                                 correct_aspect=True, scale_to_bounds=True)
        try:   # tighter packing = more texels per island
            bpy.ops.uv.pack_islands(rotate=True, margin=0.004, shape_method="CONCAVE")
        except Exception as e:
            print("pack_islands:", e)
        bpy.ops.object.mode_set(mode="OBJECT")
        img = bpy.data.images.new(self.name + "_albedo", size, size, alpha=False)
        sc.render.engine = "CYCLES"
        sc.cycles.device = "CPU"
        sc.cycles.samples = samples
        sc.render.bake.margin = 6
        for slot in ob.material_slots:
            nt = slot.material.node_tree
            n = nt.nodes.new("ShaderNodeTexImage")
            n.image = img
            nt.nodes.active = n
            n.select = True
        bpy.ops.object.bake(type="EMIT", margin=6, use_clear=True)
        img.filepath_raw = out_png
        img.file_format = "PNG"
        img.save()
        m = bpy.data.materials.new("M_" + self.name)
        nt = m.node_tree
        bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
        tx = nt.nodes.new("ShaderNodeTexImage")
        tx.image = img
        nt.links.new(tx.outputs["Color"], bsdf.inputs["Base Color"])
        bsdf.inputs["Roughness"].default_value = 0.65
        bsdf.inputs["Metallic"].default_value = 0.0
        ob.data.materials.clear()
        ob.data.materials.append(m)
        ob.data.polygons.foreach_set("material_index", [0] * len(ob.data.polygons))
        self.material = m
        return img

    # ---------------------------------------------------------- animation
    def _apply_pose(self, pose):
        for pb in self.rig.pose.bones:
            qa = pose.get(pb.name, Quaternion())
            Rr = self.rest[pb.name]
            pb.rotation_quaternion = (Rr.inverted() @ qa.to_matrix() @ Rr).to_quaternion()
        rootn = self.bones[0][0]
        loc = cv(pose.get("_loc", (0, 0, 0)))
        self.rig.pose.bones[rootn].location = self.rest[rootn].inverted() @ loc

    def clip(self, name, nframes, fn, step=2):
        rig = self.rig
        rig.animation_data_create()
        act = bpy.data.actions.new(name)
        act.use_fake_user = True
        rig.animation_data.action = act
        frames = list(range(0, nframes + 1, step))
        if frames[-1] != nframes:
            frames.append(nframes)
        rootn = self.bones[0][0]
        for f in frames:
            self._apply_pose(fn(f / nframes))
            for pb in rig.pose.bones:
                pb.keyframe_insert("rotation_quaternion", frame=f)
            rig.pose.bones[rootn].keyframe_insert("location", frame=f)
        act["nframes"] = nframes
        act["keys"] = frames
        self.meta.setdefault("clips", {})[name] = nframes
        return act

    def _minz(self):
        dg = bpy.context.evaluated_depsgraph_get()
        ev = self.body.evaluated_get(dg)
        me = ev.to_mesh()
        n = len(me.vertices)
        co = np.empty(n * 3, np.float32)
        me.vertices.foreach_get("co", co)
        ev.to_mesh_clear()
        co = co.reshape(-1, 3)
        return float(co[:, 2].min()), co

    def ground_fix(self, act, exact_from=1.01):
        """Shift the root's z per key so nothing dips under the ground; from t>=exact_from the
        lowest point is put exactly ON the ground (corpse frames)."""
        rig = self.rig
        rig.animation_data.action = act
        rootn = self.bones[0][0]
        pb = rig.pose.bones[rootn]
        Rr = self.rest[rootn]
        n = act["nframes"]
        sc = bpy.context.scene
        for f in act["keys"]:
            sc.frame_set(f)
            mz, _ = self._minz()
            dz = -mz if f / n >= exact_from else max(0.0, -mz)
            if abs(dz) < 1e-5:
                continue
            arm_loc = Rr @ Vector(pb.location) + Vector((0, 0, dz))
            pb.location = Rr.inverted() @ arm_loc
            pb.keyframe_insert("location", frame=f)
        sc.frame_set(0)

    def pose_extent(self, act, frame):
        self.rig.animation_data.action = act
        bpy.context.scene.frame_set(frame)
        _, co = self._minz()
        bpy.context.scene.frame_set(0)
        return co.min(0), co.max(0)

    def finish_clips(self, acts):
        rig = self.rig
        for act in acts:
            tr = rig.animation_data.nla_tracks.new()
            tr.name = act.name
            tr.strips.new(act.name, 0, act)
        rig.animation_data.action = None

    # ---------------------------------------------------------- export + meta
    def export(self, path):
        animated = bool(self.bones)
        bpy.ops.object.select_all(action="DESELECT")
        self.body.select_set(True)
        if animated:
            self.rig.select_set(True)
        kw = dict(filepath=path, export_format="GLB", use_selection=True, export_yup=True, export_apply=True,
                  export_materials="EXPORT", export_image_format="AUTO", export_tangents=False)
        if animated:
            kw.update(export_animations=True, export_animation_mode="NLA_TRACKS", export_skins=True,
                      export_force_sampling=True, export_frame_range=False, export_def_bones=False)
        else:
            kw.update(export_animations=False, export_skins=False)
        bpy.ops.export_scene.gltf(**kw)

    def write_meta(self, path, extra=None):
        if self.rig:
            self.rig.animation_data.action = None
            for tr in self.rig.animation_data.nla_tracks:
                tr.mute = True
        bpy.context.scene.frame_set(0)
        _, co = self._minz()
        if self.rig:
            for tr in self.rig.animation_data.nla_tracks:
                tr.mute = False
        h = float(co[:, 2].max())
        r = float(np.sqrt((co[:, 0] ** 2 + co[:, 1] ** 2)).max())
        names = self.bone_names()
        kind = "prop" if not names else ("flyer" if "body" in names else "walker")
        d = dict(kind=kind, height=round(h, 4), radius=round(r, 4), doom_height=self.height, doom_radius=self.radius,
                 doom_scale=round(self.height / h, 4), tris=self.tri_count(),
                 bones=len(self.bones), clips={k: {"frames": v + 1, "fps": 60} for k, v in self.meta.get("clips", {}).items()})
        if extra:
            d.update(extra)
        with open(path, "w") as f:
            json.dump(d, f, indent=1)
