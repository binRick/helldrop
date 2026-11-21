"""The creature roster: one builder per model.  All coordinates are char space Doom units
(x right, y forward, z up); see hdlib.py.  Designs are original role-equivalents."""
import math

from mathutils import Vector

from hdlib import Model
from rigs import (V3, add_biped_bones, add_flyer_bones, biped_clips, biped_joints, flyer_clips, hang_arms)

BUILDERS = {}


def builder(fn):
    BUILDERS[fn.__name__] = fn
    return fn


def finish(m, tex_png, max_tris=15000):
    m.assemble()
    m.decimate_to(max_tris)
    m.bake(tex_png)
    return m


def along(o, d, u, side=0.0, up=0.0):
    """Point u units along direction d from o, offset right/up (for guns)."""
    d = Vector(d).normalized()
    r = d.cross(Vector((0, 0, 1))).normalized()
    upv = r.cross(d).normalized()
    return tuple(Vector(o) + d * u + r * side + upv * up)


def yaw_of(d):
    return math.degrees(math.atan2(-d[0], d[1]))


def pitch_of(d):
    return math.degrees(math.atan2(d[2], math.hypot(d[0], d[1])))


# ------------------------------------------------------------------ shared humanoid body
def soldier_body(m, J, paint, r=1.0, belly=1.0):
    """Undersuit skin: torso, arms, legs as one connected skin tree."""
    P, W, C, N, HB = J["pelvis"], J["waist"], J["chest"], J["neck"], J["headb"]
    chains = [
        ([(P.x, P.y, P.z - 1.8, 5.6 * r), (*P, 6.2 * r), (*W, 5.7 * r * belly), (*C, 7.2 * r), (N.x, N.y, N.z - 1.6, 6.2 * r),
          (*N, 3.0 * r), (*HB, 2.8 * r)], ["hips", "hips", "spine", "chest", "chest", "neck"]),
    ]
    for s in ("R", "L"):
        sh, el, wr, tip = J["sh." + s], J["el." + s], J["wr." + s], J["tip." + s]
        hp, kn, an, to = J["hip." + s], J["knee." + s], J["ankle." + s], J["toe." + s]
        chains.append(([(N.x, N.y, N.z - 1.6, 6.2 * r), (*sh, 3.5 * r), (*el, 2.7 * r), (*wr, 2.2 * r), (*tip, 1.9 * r)],
                       ["chest", "upperarm." + s, "forearm." + s, "hand." + s]))
        chains.append(([(P.x, P.y, P.z - 1.8, 5.6 * r), (*hp, 4.6 * r), (*kn, 3.2 * r), (*an, 2.5 * r), (*to, 1.9 * r)],
                       ["hips", "thigh." + s, "shin." + s, "foot." + s]))
    return m.skin(chains, paint, subdiv=2, displace=0.25, dscale=3.0, name="suit")


def limb_tube(m, a, b, ra, rb, paint, bone, segs=10, inset=0.0):
    a, b = Vector(a), Vector(b)
    d = (b - a).normalized()
    return m.tube([a + d * inset, b - d * inset], [ra, rb], paint, bone, segs=segs)


# ================================================================== 1 marine
@builder
def marine(name, tex):
    H = 56.0
    m = Model(name, H, 16)
    m.paint("armor", (0.40, 0.45, 0.22), color2=(0.29, 0.33, 0.16), pattern="mottle", pscale=5, wear=0.8,
            wear_color=(0.70, 0.68, 0.58), grime=0.6, seams=(4.5, 0.55))
    m.paint("plate2", (0.31, 0.35, 0.18), wear=0.7, wear_color=(0.66, 0.64, 0.54), seams=(3.0, 0.4))
    m.paint("suit", (0.15, 0.16, 0.17), color2=(0.10, 0.10, 0.11), pattern="plates", pscale=2.5, wear=0.1)
    m.paint("metal", (0.23, 0.24, 0.26), wear=0.9, wear_color=(0.68, 0.69, 0.72), grime=0.5)
    m.paint("dark", (0.10, 0.10, 0.11), wear=0.4, wear_color=(0.35, 0.35, 0.36))
    m.paint("visor", (1.0, 0.50, 0.06), glow=True)
    m.paint("light", (1.0, 0.62, 0.12), glow=True)
    m.paint("leather", (0.25, 0.18, 0.11), wear=0.3, wear_color=(0.4, 0.3, 0.2))
    J = biped_joints(H, hip=0.5, shw=0.2, chest=0.74, shoulder=0.81, neck=0.85, headb=0.865, headtop=0.995)
    # rifle carried at the right hip, angled slightly across the body
    O, D = V3(8.2, 3.0, 33.6), V3(-5.0, 32.0, 2.2)
    g = lambda u, s=0.0, up=0.0: along(O, D, u, s, up)  # noqa: E731
    J["el.R"], J["wr.R"], J["tip.R"] = V3(12.8, 0.6, 36.2), V3(9.6, 7.0, 32.2), V3(8.0, 9.4, 31.4)
    lh = V3(*g(18.5, 0, -1.6))
    J["el.L"], J["wr.L"], J["tip.L"] = V3(-10.2, 7.5, 37.2), lh + V3(-2.6, -2.4, 0.2), lh + V3(0.4, 0.4, -0.2)
    add_biped_bones(m, J)
    soldier_body(m, J, "suit")
    hb = J["headb"]
    hc = hb + V3(0, 0.6, 3.9)
    # helmet: shell, visor, cheek guards, crest, rebreather
    m.ellip(hc + V3(0, -0.4, 0.4), (4.5, 5.0, 4.5), "armor", "head", segs=(20, 14), name="helmet")
    m.ellip(hc + V3(0, 3.2, -0.2), (3.6, 2.2, 1.55), "visor", "head", segs=(16, 8), name="visor")
    m.box(hc + V3(0, 0.6, 3.9), (1.6, 8.6, 1.4), "plate2", "head", bevel=0.4, name="crest")
    m.sym(m.box, hc + V3(4.0, 0.9, -1.2), (1.6, 5.0, 4.2), "plate2", "head", rot=(0, 0, 8), bevel=0.3)
    m.box(hc + V3(0, 4.2, -2.6), (4.0, 2.2, 2.4), "metal", "head", bevel=0.35, name="rebreather")
    m.sym(m.cyl, tuple(hc + V3(1.4, 5.0, -2.8)), tuple(hc + V3(1.6, 5.8, -3.0)), 0.7, "dark", "head", segs=8)
    # torso armour
    C, W, P = J["chest"], J["waist"], J["pelvis"]
    m.box(C + V3(0, 1.2, 1.2), (17.0, 13.6, 10.4), "armor", "chest", taper=(1.07, 1.0), bevel=0.28, name="cuirass")
    m.box(C + V3(0, 7.9, 0.4), (9.0, 1.4, 6.5), "plate2", "chest", bevel=0.3, name="sternum")
    m.sym(m.box, C + V3(3.6, 8.3, -2.6), (2.4, 1.2, 1.2), "light", "chest", bevel=0.2)
    m.box(C + V3(0, 1.0, 5.8), (11.0, 10.0, 2.2), "metal", "chest", bevel=0.4, name="collar")
    m.box(W + V3(0, 1.2, 0.2), (13.4, 11.6, 4.4), "plate2", "spine", bevel=0.3, name="abs")
    m.box(W + V3(0, 1.0, -3.6), (12.6, 11.4, 3.4), "armor", "hips", bevel=0.3, name="abs2")
    m.box(P + V3(0, 0.3, -0.4), (14.0, 11.8, 2.3), "leather", "hips", bevel=0.3, name="belt")
    for x, y in ((-5.4, 5.6), (5.4, 5.6), (-6.8, 1.5), (6.8, 1.5)):
        m.box(P + V3(x, y, -0.6), (2.6, 2.0, 3.2), "leather", "hips", bevel=0.25)
    m.box(P + V3(0, 5.4, -3.6), (5.0, 1.6, 5.0), "plate2", "hips", taper=(0.6, 1), bevel=0.3, name="codpiece")
    m.box(C + V3(0, -7.6, 0.4), (11.0, 4.6, 10.5), "metal", "chest", bevel=0.25, name="pack")
    m.sym(m.cyl, tuple(C + V3(3.2, -10.4, -4.6)), tuple(C + V3(3.2, -10.4, 5.2)), 1.7, "dark", "chest", segs=12)
    m.box(C + V3(0, -10.3, 0.0), (2.0, 0.8, 7.0), "light", "chest", bevel=0.2)
    # pauldrons + arms
    for s, sx in (("R", 1), ("L", -1)):
        sh, el, wr, tip = J["sh." + s], J["el." + s], J["wr." + s], J["tip." + s]
        m.box(sh + V3(sx * 1.4, 0.0, 2.4), (8.4, 10.4, 3.4), "armor", "upperarm." + s, rot=(0, sx * 20, 0),
              taper=(0.72, 0.8), bevel=0.3, name="pauldron")
        m.box(sh + V3(sx * 3.6, 0.0, -0.6), (5.4, 9.6, 2.4), "plate2", "upperarm." + s, rot=(0, sx * 48, 0),
              taper=(0.8, 0.9), bevel=0.3, name="pauldron2")
        m.box(sh + V3(sx * 0.4, 0.0, 4.3), (4.2, 8.0, 1.0), "light" if s == "R" else "metal", "upperarm." + s,
              rot=(0, sx * 20, 0), bevel=0.3, name="pauldron_trim")
        limb_tube(m, sh + (el - sh) * 0.45, el, 3.1, 2.9, "plate2", "upperarm." + s, inset=0.0)
        limb_tube(m, el + (wr - el) * 0.12, wr, 3.15, 2.85, "armor", "forearm." + s)
        m.ellip(el, (3.0, 3.0, 3.0), "metal", "forearm." + s, segs=(12, 8), name="elbow")
        m.ellip(tip + (wr - tip) * 0.35, (2.4, 2.7, 2.3), "dark", "hand." + s, segs=(12, 8), name="glove")
    # legs
    for s, sx in (("R", 1), ("L", -1)):
        hp, kn, an, to = J["hip." + s], J["knee." + s], J["ankle." + s], J["toe." + s]
        limb_tube(m, hp + (kn - hp) * 0.15, kn + (hp - kn) * 0.12, 4.9, 3.9, "armor", "thigh." + s, segs=12)
        m.ellip(kn + V3(0, 2.3, 0.6), (3.0, 2.0, 3.3), "plate2", "shin." + s, segs=(12, 8), name="kneepad")
        limb_tube(m, kn + (an - kn) * 0.15, an + (kn - an) * 0.2, 3.5, 3.0, "armor", "shin." + s, segs=12)
        m.box((an.x, 2.6, 2.3), (5.2, 10.6, 4.6), "dark", "foot." + s, bevel=0.3, taper=(0.92, 0.85), name="boot")
        m.box((an.x, 3.4, 0.6), (5.6, 11.4, 1.3), "metal", "foot." + s, bevel=0.3, name="sole")
    for s, sx in (("R", 1), ("L", -1)):
        hp, kn = J["hip." + s], J["knee." + s]
        q = hp.lerp(kn, 0.4)
        m.box(q + V3(sx * 4.4, 0.6, 0), (2.2, 4.4, 5.0), "leather", "thigh." + s, bevel=0.3, name="thighpouch")
        m.box(q + V3(sx * 4.4, 0.6, 2.0), (2.6, 4.8, 0.8), "plate2", "thigh." + s, bevel=0.3)
    m.cyl(tuple(C + V3(-4.0, -9.6, 4.0)), tuple(C + V3(-4.6, -11.0, 16.0)), 0.25, "dark", "chest", segs=6, name="antenna")
    m.ellip(C + V3(-4.6, -11.1, 16.2), (0.6, 0.6, 0.6), "light", "chest", segs=(8, 6))
    for x in (-3.2, -1.0):
        m.ellip(J["pelvis"] + V3(x - 3.0, 6.4, -2.6), (1.1, 1.1, 1.4), "plate2", "hips", segs=(10, 8), name="grenade")
    # rifle (rigid on the right hand)
    yaw, pit = yaw_of(D), pitch_of(D)
    rot = (pit, 0, yaw)
    m.box(g(10.0, 0, 0.4), (3.0, 17.0, 4.2), "metal", "hand.R", rot=rot, bevel=0.2, name="receiver")
    m.box(g(10.0, 0, 2.9), (1.6, 12.0, 1.2), "dark", "hand.R", rot=rot, bevel=0.3, name="rail")
    m.box(g(-0.5, 0, -0.2), (2.4, 7.0, 3.4), "dark", "hand.R", rot=rot, bevel=0.3, taper=(1, 0.8), name="stock")
    m.box(g(6.6, 0, -2.6), (1.8, 2.2, 4.2), "dark", "hand.R", rot=(pit - 18, 0, yaw), bevel=0.25, name="grip")
    m.box(g(12.5, 0, -3.0), (2.2, 3.2, 5.4), "metal", "hand.R", rot=(pit + 12, 0, yaw), bevel=0.2, name="mag")
    m.box(g(18.4, 0, -0.6), (3.4, 6.0, 3.0), "dark", "hand.R", rot=rot, bevel=0.3, name="foregrip")
    m.cyl(g(18.0, 0, 1.0), g(31.5, 0, 1.0), 0.85, "metal", "hand.R", segs=10, name="barrel")
    m.cyl(g(29.0, 0, 1.0), g(32.5, 0, 1.0), 1.25, "dark", "hand.R", segs=10, name="muzzle")
    m.cyl(g(7.0, 0, 3.9), g(13.0, 0, 3.9), 1.1, "dark", "hand.R", segs=10, name="scope")
    m.box(g(10.0, 1.65, 0.6), (0.4, 9.0, 0.7), "light", "hand.R", rot=rot, bevel=0.0, name="strip")
    m.box(g(10.0, -1.65, 0.6), (0.4, 9.0, 0.7), "light", "hand.R", rot=rot, bevel=0.0, name="strip2")
    finish(m, tex)
    biped_clips(m, dict(gun=True, attack="gun", death="back", stride=24, knee=46, leglen=28, arm_fwd=105))
    return m


# ------------------------------------------------------------------ shared helpers
def organic_body(m, J, paint, R, digi=False, subdiv=2, displace=0.5, dscale=4.0, name="body", head=None):
    """Connected skin tree: torso (+ optional head), arms, legs.  R: radii by joint."""
    P, W, C, N, HB = J["pelvis"], J["waist"], J["chest"], J["neck"], J["headb"]
    UC = C.lerp(N, 0.6)
    PB = P - V3(0, 0, R["pel"] * 0.35)
    torso = [(*PB, R["pel"] * 0.85), (*P, R["pel"]), (*W, R["waist"]), (*C, R["chest"]), (*UC, R["uc"]),
             (*N, R["neck"]), (*HB, R["hb"])]
    tb = ["hips", "hips", "spine", "chest", "chest", "neck"]
    if head:
        for p, r in head:
            torso.append((*p, r))
            tb.append("head")
    chains = [(torso, tb)]
    for s in ("R", "L"):
        chains.append(([(*UC, R["uc"]), (*J["sh." + s], R["sh"]), (*J["el." + s], R["el"]), (*J["wr." + s], R["wr"]),
                        (*J["tip." + s], R["tip"])], ["chest", "upperarm." + s, "forearm." + s, "hand." + s]))
        leg = [(*PB, R["pel"] * 0.85), (*J["hip." + s], R["hip"]), (*J["knee." + s], R["knee"])]
        lb = ["hips", "thigh." + s]
        if digi:
            leg.append((*J["hock." + s], R["hock"]))
            lb.append("shin." + s)
            lb += ["meta." + s, "foot." + s]
        else:
            lb += ["shin." + s, "foot." + s]
        leg += [(*J["ankle." + s], R["ankle"]), (*J["toe." + s], R["toe"])]
        chains.append((leg, lb))
    return m.skin(chains, paint, subdiv=subdiv, displace=displace, dscale=dscale, name=name)


def claws(m, J, side, paint, n=3, length=3.0, r=0.5, spread=22.0, curl=-18.0):
    wr, tip = J["wr." + side], J["tip." + side]
    d = (tip - wr).normalized()
    sx = 1 if side == "R" else -1
    for i in range(n):
        a = math.radians((i - (n - 1) / 2) * spread)
        side_v = d.cross(Vector((0, 0, 1))).normalized()
        dd = (d * math.cos(a) + side_v * math.sin(a) * sx).normalized()
        base = tip - d * 0.6
        m.horn(base, dd, length, r, paint, "hand." + side, curl=(curl, 0, 0), segs=6, steps=4, name="claw")


def muscle(m, c, radii, paint, rot=(0, 0, 0)):
    return m.ellip(c, radii, paint, None, rot=rot, segs=(14, 10), name="muscle")


def gun_arms(J, g, grip_u=6.0, fore_u=18.5):
    """Arms holding a long gun at the right hip (g(u, side, up) walks along the gun)."""
    shR, shL = J["sh.R"], J["sh.L"]
    gp = V3(*g(grip_u, 0, -2.6))
    J["tip.R"] = gp + V3(0.5, -0.3, -0.6)
    J["wr.R"] = J["tip.R"] + V3(1.5, -2.4, 0.8)
    J["el.R"] = shR + V3(1.6, 0.6, -9.2)
    lh = V3(*g(fore_u, 0, -1.6))
    J["tip.L"] = lh + V3(0.4, 0.4, -0.2)
    J["wr.L"] = lh + V3(-2.6, -2.4, 0.2)
    J["el.L"] = shL + V3(1.0, 7.5, -8.2)


def long_gun(m, g, D, kind, P):
    """Rigid weapon on hand.R.  P: dict of paint keys (metal, dark, wood, light)."""
    rot = (pitch_of(D), 0, yaw_of(D))
    if kind == "rifle":
        m.box(g(10.0, 0, 0.4), (3.0, 17.0, 4.0), P["metal"], "hand.R", rot=rot, bevel=0.2, name="receiver")
        m.box(g(-0.5, 0, -0.2), (2.4, 7.0, 3.4), P["wood"], "hand.R", rot=rot, bevel=0.3, taper=(1, 0.8), name="stock")
        m.box(g(6.6, 0, -2.6), (1.8, 2.2, 4.2), P["dark"], "hand.R", rot=(rot[0] - 18, 0, rot[2]), bevel=0.25)
        m.box(g(12.0, 0, -3.4), (1.8, 2.6, 5.8), P["dark"], "hand.R", rot=(rot[0] + 22, 0, rot[2]), bevel=0.2)
        m.box(g(18.0, 0, -0.4), (3.2, 7.0, 2.8), P["wood"], "hand.R", rot=rot, bevel=0.3)
        m.cyl(g(17.0, 0, 1.0), g(30.5, 0, 1.0), 0.8, P["metal"], "hand.R", segs=10)
        m.cyl(g(29.0, 0, 1.0), g(31.5, 0, 1.0), 1.1, P["dark"], "hand.R", segs=10)
        m.cyl(g(18.0, 0, 2.6), g(25.0, 0, 2.6), 0.5, P["metal"], "hand.R", segs=8)
    elif kind == "shotgun":
        m.box(g(7.5, 0, 0.6), (2.8, 11.0, 3.8), P["metal"], "hand.R", rot=rot, bevel=0.2, name="receiver")
        m.box(g(-1.5, 0, -0.6), (2.6, 9.0, 3.6), P["wood"], "hand.R", rot=(rot[0] - 6, 0, rot[2]), bevel=0.35, taper=(1, 0.75))
        m.box(g(6.0, 0, -2.4), (1.8, 2.2, 4.0), P["dark"], "hand.R", rot=(rot[0] - 18, 0, rot[2]), bevel=0.25)
        m.cyl(g(12.0, 0.75, 1.4), g(33.0, 0.75, 1.4), 0.95, P["metal"], "hand.R", segs=10)
        m.cyl(g(12.0, -0.75, 1.4), g(33.0, -0.75, 1.4), 0.95, P["metal"], "hand.R", segs=10)
        m.cyl(g(12.0, 0, -0.5), g(30.0, 0, -0.5), 0.9, P["dark"], "hand.R", segs=10)
        m.box(g(19.0, 0, -0.3), (3.4, 7.5, 3.0), P["wood"], "hand.R", rot=rot, bevel=0.35, name="pump")
        m.box(g(7.5, 1.45, 0.8), (0.3, 6.0, 0.8), P["light"], "hand.R", rot=rot, bevel=0.0)
    elif kind == "chaingun":
        m.box(g(8.0, 0, 0.4), (6.0, 14.0, 6.0), P["metal"], "hand.R", rot=rot, bevel=0.2, name="housing")
        m.box(g(8.0, -3.6, 0.4), (2.2, 8.0, 4.5), P["light"], "hand.R", rot=rot, bevel=0.25, name="drum")
        m.box(g(4.0, 0, -3.4), (2.0, 2.4, 4.2), P["dark"], "hand.R", rot=(rot[0] - 15, 0, rot[2]), bevel=0.25)
        m.box(g(8.0, 0, 4.4), (1.2, 8.0, 2.0), P["dark"], "hand.R", rot=rot, bevel=0.3, name="handle")
        for k in range(6):
            a = 2 * math.pi * k / 6
            s, u = 1.75 * math.cos(a), 1.75 * math.sin(a) + 0.4
            m.cyl(g(14.0, s, u), g(31.0, s, u), 0.6, P["dark"], "hand.R", segs=8)
        m.cyl(g(14.5, 0, 0.4), g(16.5, 0, 0.4), 2.8, P["metal"], "hand.R", segs=14)
        m.cyl(g(24.0, 0, 0.4), g(25.2, 0, 0.4), 2.6, P["metal"], "hand.R", segs=14)
        m.cyl(g(30.0, 0, 0.4), g(31.4, 0, 0.4), 2.6, P["metal"], "hand.R", segs=14)


# ================================================================== 2-4 the fallen troopers
def trooper(name, tex, kind):
    H = 56.0
    m = Model(name, H, 20)
    if kind == "zombie":
        m.paint("cloth", (0.55, 0.47, 0.34), color2=(0.40, 0.34, 0.24), pattern="camo", pscale=4, wear=0.2,
                grime=0.9, spots=((0.40, 0.03, 0.02), 2.6, 1.0), zband=(24.0, 28.0, (0.32, 0.29, 0.22)))
        m.paint("skin", (0.64, 0.66, 0.60), color2=(0.48, 0.42, 0.42), pattern="veins", pscale=2.5, pamount=0.3,
                wear=0.0, grime=0.7, spots=((0.35, 0.02, 0.02), 1.8, 0.8))
        m.paint("vest", (0.30, 0.25, 0.18), wear=0.4, grime=0.8, spots=((0.4, 0.02, 0.02), 2.5, 0.9))
        m.paint("metal", (0.30, 0.25, 0.20), wear=0.8, wear_color=(0.55, 0.40, 0.28), grime=0.8)
        m.paint("dark", (0.12, 0.11, 0.10), wear=0.4)
        m.paint("wood", (0.38, 0.24, 0.13), wear=0.4)
        m.paint("eye", (1.0, 0.25, 0.08), glow=True)
        m.paint("light", (1.0, 0.25, 0.08), glow=True)
        m.paint("boot", (0.16, 0.13, 0.10), wear=0.4)
        hunch, r, gun = 0.07, 1.0, "rifle"
    elif kind == "shotgunner":
        m.paint("cloth", (0.22, 0.22, 0.24), color2=(0.16, 0.16, 0.17), pattern="mottle", pscale=4, wear=0.2, grime=0.7,
                spots=((0.3, 0.02, 0.02), 3.0, 0.6))
        m.paint("skin", (0.66, 0.60, 0.54), color2=(0.45, 0.33, 0.30), pattern="veins", pscale=2.5, pamount=0.6,
                wear=0.0, grime=0.7, spots=((0.35, 0.02, 0.02), 1.8, 0.6))
        m.paint("vest", (0.13, 0.13, 0.14), wear=0.6, wear_color=(0.42, 0.42, 0.44), seams=(3.5, 0.5))
        m.paint("red", (0.72, 0.07, 0.05), wear=0.5, wear_color=(0.9, 0.4, 0.3))
        m.paint("metal", (0.20, 0.21, 0.23), wear=0.8, wear_color=(0.62, 0.62, 0.65))
        m.paint("dark", (0.10, 0.10, 0.11), wear=0.4)
        m.paint("wood", (0.42, 0.22, 0.10), wear=0.5)
        m.paint("eye", (1.0, 0.18, 0.06), glow=True)
        m.paint("light", (1.0, 0.18, 0.06), glow=True)
        m.paint("boot", (0.10, 0.09, 0.09), wear=0.4)
        hunch, r, gun = 0.05, 1.0, "shotgun"
    else:
        m.paint("cloth", (0.12, 0.12, 0.13), color2=(0.08, 0.08, 0.09), pattern="plates", pscale=2.5, wear=0.2)
        m.paint("skin", (0.58, 0.52, 0.48), wear=0.0, grime=0.7)
        m.paint("vest", (0.66, 0.09, 0.07), color2=(0.45, 0.05, 0.04), pattern="mottle", pscale=5, wear=0.8,
                wear_color=(0.80, 0.62, 0.52), seams=(4.0, 0.55), grime=0.6)
        m.paint("red", (0.50, 0.06, 0.05), wear=0.6, wear_color=(0.8, 0.5, 0.4), seams=(3.0, 0.4))
        m.paint("metal", (0.22, 0.23, 0.25), wear=0.9, wear_color=(0.66, 0.66, 0.70))
        m.paint("dark", (0.09, 0.09, 0.10), wear=0.4)
        m.paint("wood", (0.18, 0.18, 0.19), wear=0.5)
        m.paint("eye", (1.0, 0.2, 0.05), glow=True)
        m.paint("light", (0.80, 0.60, 0.20), wear=0.5, wear_color=(1.0, 0.85, 0.45))   # brass drum
        m.paint("boot", (0.10, 0.10, 0.11), wear=0.4)
        hunch, r, gun = 0.03, 1.22, "chaingun"
    J = biped_joints(H, hip=0.5, shw=0.19 * (1.12 if r > 1.1 else 1.0), chest=0.74, shoulder=0.80, neck=0.85,
                     headb=0.865, headtop=0.985, hunch=hunch, stance=0.03 if r > 1.1 else 0.02)
    hy = J["chest"].y
    O = V3(8.6 * (1.1 if r > 1.1 else 1.0), 3.0 + hy, 33.0 if gun != "chaingun" else 31.5)
    D = V3(-4.6, 32.0, 1.6)
    g = lambda u, s=0.0, up=0.0: along(O, D, u, s, up)  # noqa: E731
    gun_arms(J, g, 6.0 if gun != "chaingun" else 4.0, 18.5 if gun != "chaingun" else 9.0)
    if gun == "chaingun":
        J["el.L"] = J["sh.L"] + V3(0.5, 5.0, -9.0)
    add_biped_bones(m, J)
    soldier_body(m, J, "cloth", r=r, belly=1.0)
    hb = J["headb"]
    hc = hb + V3(0, 0.6, 3.7)
    C, W, P = J["chest"], J["waist"], J["pelvis"]
    if kind == "zombie":
        m.ellip(hc, (3.7, 4.3, 4.3), "skin", "head", segs=(18, 12), noise=0.05, name="head")
        m.ellip(hc + V3(0, 2.6, -2.2), (2.8, 2.4, 1.8), "skin", "head", segs=(12, 8), name="jaw")
        m.box(hc + V3(0, 3.5, 0.9), (5.4, 1.4, 1.3), "skin", "head", bevel=0.4, name="brow")
        m.sym(m.ellip, hc + V3(1.4, 3.9, 0.2), (0.75, 0.4, 0.5), "eye", "head", segs=(8, 6))
        m.box(hc + V3(0, 4.1, -2.1), (2.6, 0.6, 0.8), "dark", "head", bevel=0.2, name="mouth")
        m.box(C + V3(0, 1.0, 0.6), (14.0, 12.0, 9.0), "vest", "chest", bevel=0.3, taper=(1.05, 1.0), name="vest")
        for x in (-3.5, 0.0, 3.5):
            m.box(C + V3(x, 6.6, -2.4), (2.6, 1.6, 3.2), "vest", "chest", bevel=0.3)
        m.box(P + V3(0, 0.3, -0.2), (13.0, 11.4, 2.0), "dark", "hips", bevel=0.3, name="belt")
        for k, (x, y, rz) in enumerate(((-4.5, 5.0, 10), (3.0, 5.4, -8), (5.5, -4.0, 20), (-3.0, -5.2, -14))):
            m.box(P + V3(x, y, -4.5), (3.4, 0.5, 6.5), "cloth", "hips", rot=(8 if y > 0 else -8, 0, rz), bevel=0.2,
                  name="tatter")
        for s, sx in (("R", 1), ("L", -1)):
            el, wr, tip = J["el." + s], J["wr." + s], J["tip." + s]
            limb_tube(m, el + (wr - el) * 0.05, wr, 2.5, 2.1, "skin", "forearm." + s)
            m.ellip(tip + (wr - tip) * 0.3, (2.0, 2.3, 1.9), "skin", "hand." + s, segs=(10, 8))
            m.ellip(J["sh." + s] + V3(sx * 1.0, 0, 0.6), (3.8, 4.2, 3.0), "cloth", "upperarm." + s, segs=(12, 8))
    elif kind == "shotgunner":
        m.ellip(hc, (3.6, 4.2, 4.2), "skin", "head", segs=(18, 12), noise=0.04, name="head")
        m.box(hc + V3(0, 2.6, -2.0), (5.6, 3.4, 3.2), "dark", "head", bevel=0.35, name="mask")
        m.sym(m.cyl, tuple(hc + V3(1.9, 4.2, -2.6)), tuple(hc + V3(2.4, 5.2, -3.3)), 0.8, "metal", "head", segs=8)
        m.box(hc + V3(0, 3.7, 0.8), (6.6, 1.2, 1.7), "dark", "head", bevel=0.3, name="goggle_band")
        m.sym(m.cyl, tuple(hc + V3(1.5, 3.8, 0.8)), tuple(hc + V3(1.5, 4.7, 0.8)), 1.0, "eye", "head", segs=10)
        m.box(hc + V3(0, -0.4, 3.0), (2.0, 8.0, 1.6), "red", "head", bevel=0.4, name="mohawk")
        m.box(C + V3(0, 1.0, 0.8), (15.4, 13.0, 10.4), "vest", "chest", bevel=0.28, taper=(1.06, 1.0), name="vest")
        m.box(C + V3(0, 7.5, 0.6), (3.2, 0.8, 9.0), "red", "chest", bevel=0.3, name="stripe")
        m.box(W + V3(0, 1.0, -0.6), (13.0, 11.6, 5.0), "vest", "spine", bevel=0.3)
        m.box(P + V3(0, 0.3, -0.2), (13.6, 11.6, 2.2), "dark", "hips", bevel=0.3, name="belt")
        for x in (-5.0, -2.5, 2.5, 5.0):
            m.cyl((x, 5.9, P.z - 1.0), (x, 5.9, P.z + 0.6), 0.55, "red", "hips", segs=6)
        for s, sx in (("R", 1), ("L", -1)):
            sh, el, wr, tip = J["sh." + s], J["el." + s], J["wr." + s], J["tip." + s]
            m.box(sh + V3(sx * 1.4, 0.0, 2.0), (7.0, 8.6, 2.6), "vest", "upperarm." + s, rot=(0, sx * 22, 0),
                  taper=(0.75, 0.85), bevel=0.3)
            m.box(sh + V3(sx * 1.6, 0.0, 3.4), (4.6, 7.0, 0.7), "red", "upperarm." + s, rot=(0, sx * 22, 0), bevel=0.3)
            limb_tube(m, el + (wr - el) * 0.1, wr, 2.7, 2.3, "vest", "forearm." + s)
            m.ellip(tip + (wr - tip) * 0.3, (2.0, 2.3, 1.9), "dark", "hand." + s, segs=(10, 8))
    else:
        m.ellip(hc + V3(0, -0.2, 0.3), (4.6, 5.0, 4.5), "vest", "head", segs=(20, 14), name="helmet")
        m.box(hc + V3(0, 3.6, -0.6), (6.4, 2.4, 3.8), "red", "head", bevel=0.35, name="faceplate")
        m.box(hc + V3(0, 4.85, 0.2), (5.0, 0.4, 0.8), "eye", "head", bevel=0.0, name="slit")
        m.sym(m.cyl, tuple(hc + V3(2.2, 4.6, -1.8)), tuple(hc + V3(2.6, 5.6, -2.0)), 0.7, "dark", "head", segs=8)
        m.box(C + V3(0, 1.2, 1.2), (20.0, 15.6, 11.5), "vest", "chest", bevel=0.25, taper=(1.08, 1.0), name="cuirass")
        m.box(C + V3(0, 8.9, 0.2), (8.0, 1.2, 7.0), "red", "chest", bevel=0.3)
        m.box(W + V3(0, 1.3, -0.2), (16.0, 13.4, 5.0), "red", "spine", bevel=0.3)
        m.box(P + V3(0, 0.3, -0.4), (16.0, 13.0, 3.0), "dark", "hips", bevel=0.3, name="belt")
        m.box(C + V3(0, -9.0, -1.0), (12.0, 5.0, 12.0), "metal", "chest", bevel=0.25, name="ammo_pack")
        # ammo belt: from the pack, round the right hip, into the gun's drum
        belt = [C + V3(5.0, -9.0, -5.0), C + V3(10.5, -5.0, -9.0), V3(*g(4.0, -3.6, -1.0)), V3(*g(8.0, -3.6, 0.4))]
        m.tube(belt, [1.2] * 4, "light", "hand.R", segs=6, name="belt")
        for s, sx in (("R", 1), ("L", -1)):
            sh, el, wr, tip = J["sh." + s], J["el." + s], J["wr." + s], J["tip." + s]
            m.box(sh + V3(sx * 1.6, 0.0, 2.6), (10.0, 11.0, 3.6), "vest", "upperarm." + s, rot=(0, sx * 18, 0),
                  taper=(0.72, 0.8), bevel=0.3)
            m.box(sh + V3(sx * 4.0, 0.0, -1.0), (6.0, 10.0, 2.6), "red", "upperarm." + s, rot=(0, sx * 46, 0),
                  taper=(0.8, 0.9), bevel=0.3)
            limb_tube(m, el + (wr - el) * 0.1, wr, 3.6, 3.1, "vest", "forearm." + s)
            m.ellip(tip + (wr - tip) * 0.3, (2.5, 2.8, 2.4), "dark", "hand." + s, segs=(10, 8))
    for s in ("R", "L"):
        hp, kn, an = J["hip." + s], J["knee." + s], J["ankle." + s]
        if kind == "chaingunner":
            limb_tube(m, hp + (kn - hp) * 0.15, kn + (hp - kn) * 0.12, 5.6, 4.6, "red", "thigh." + s, segs=12)
            limb_tube(m, kn + (an - kn) * 0.12, an + (kn - an) * 0.25, 4.2, 3.6, "vest", "shin." + s, segs=12)
        m.ellip(kn + V3(0, 2.3 * r, 0.6), (2.8 * r, 1.8 * r, 3.0), "vest" if kind != "zombie" else "boot",
                "shin." + s, segs=(12, 8))
        m.box((an.x, 2.6, 2.3), (5.0 * r, 10.2, 4.4), "boot", "foot." + s, bevel=0.3, taper=(0.92, 0.85))
        m.box((an.x, 3.2, 0.6), (5.4 * r, 11.0, 1.2), "dark", "foot." + s, bevel=0.3)
    long_gun(m, g, D, gun, dict(metal="metal", dark="dark", wood="wood", light="light"))
    finish(m, tex)
    biped_clips(m, dict(gun=True, attack="gun", death="back", stride=22,
                        knee=42, leglen=28, arm_fwd=105, lean_walk=8 if kind == "zombie" else 4,
                        twist=9 if kind == "zombie" else 6, walk_frames=72 if kind == "zombie" else 60))
    return m


@builder
def zombie(name, tex):
    return trooper(name, tex, "zombie")


@builder
def shotgunner(name, tex):
    return trooper(name, tex, "shotgunner")


@builder
def chaingunner(name, tex):
    return trooper(name, tex, "chaingunner")


# ================================================================== 5 imp-role: the Ashling
@builder
def imp(name, tex):
    H = 56.0
    m = Model(name, H, 20)
    m.paint("skin", (0.50, 0.31, 0.17), color2=(0.30, 0.17, 0.09), pattern="plates", pscale=2.6, wear=0.0,
            grime=0.8, grime_color=(0.12, 0.05, 0.03), seams=(1.8, 0.45), pointy=0.9)
    m.paint("spike", (0.86, 0.80, 0.64), wear=0.0, grime=0.9, grime_color=(0.28, 0.14, 0.06), zband=None)
    m.paint("claw", (0.14, 0.11, 0.09), wear=0.3, wear_color=(0.4, 0.35, 0.3))
    m.paint("glow", (1.0, 0.42, 0.04), glow=True)
    m.paint("eye", (1.0, 0.86, 0.2), glow=True)
    m.paint("mouth", (0.30, 0.04, 0.03), wear=0, grime=0.4)
    J = biped_joints(H, hip=0.47, digi=True, hunch=0.09, shw=0.165, chest=0.71, shoulder=0.78, neck=0.82, headb=0.83,
                     headtop=0.95)
    hang_arms(J, H, ua=0.2, fa=0.2, hand=0.07, out=16, fwd=16, elbow=34)
    hb = J["headb"]
    hc = hb + V3(0, 1.4, 3.4)
    hinge, chin = hb + V3(0, 1.6, 1.4), hb + V3(0, 6.2, -0.8)
    add_biped_bones(m, J, digi=True, jaw=(hinge, chin))
    organic_body(m, J, "skin", dict(pel=5.0, waist=4.0, chest=6.2, uc=6.4, neck=2.7, hb=2.6, sh=3.0, el=2.0, wr=1.7,
                                    tip=1.5, hip=3.9, knee=2.8, hock=1.9, ankle=1.5, toe=1.3), digi=True, displace=0.3,
                 dscale=2.5)
    C = J["chest"]
    for s, sx in (("R", 1), ("L", -1)):
        sh, el = J["sh." + s], J["el." + s]
        muscle(m, C + V3(sx * 3.0, 4.0, 0.8), (3.4, 2.2, 3.0), "skin")
        muscle(m, sh + V3(sx * 0.8, 0, -0.5), (3.3, 3.3, 3.6), "skin")
        muscle(m, sh.lerp(el, 0.5) + V3(0, 0.8, 0), (2.3, 2.3, 3.6), "skin")
        muscle(m, J["knee." + s] + V3(0, -2.5, -4.0), (2.4, 2.6, 4.0), "skin")
    m.ellip(hc, (3.5, 4.4, 3.5), "skin", "head", segs=(18, 12), noise=0.06, name="skull")
    m.box(hc + V3(0, 3.0, 1.2), (6.2, 2.2, 1.6), "skin", "head", bevel=0.45, rot=(-12, 0, 0), name="brow")
    m.sym(m.ellip, hc + V3(1.45, 4.0, 0.25), (0.95, 0.5, 0.5), "eye", "head", segs=(10, 6))
    m.ellip(hc + V3(0, 3.0, -1.4), (2.6, 2.6, 1.4), "mouth", "head", segs=(12, 8), name="maw")
    for i in range(5):
        x = (i - 2) * 1.0
        m.horn(hc + V3(x, 4.4 - abs(x) * 0.3, -1.2), (0, 0.2, -1), 1.2, 0.32, "spike", "head", segs=5, steps=2)
    m.ellip(hinge.lerp(chin, 0.62) + V3(0, 0, -0.6), (2.6, 3.0, 1.3), "skin", "jaw", segs=(12, 8), name="jaw")
    for i in range(4):
        x = (i - 1.5) * 1.1
        m.horn(chin + V3(x, -0.5, 0.2), (0, 0.1, 1), 1.2, 0.3, "spike", "jaw", segs=5, steps=2)
    m.sym(m.horn, hc + V3(2.0, -0.5, 2.0), (0.5, -0.8, 0.7), 6.5, 0.95, "spike", "head", curl=(-14, 0, 0), name="horn")
    m.sym(m.horn, hc + V3(1.0, -1.8, 2.6), (0.2, -1.0, 0.5), 4.0, 0.6, "spike", "head", curl=(-10, 0, 0))
    # shoulder / back / forearm spikes
    for s, sx in (("R", 1), ("L", -1)):
        sh, el, wr = J["sh." + s], J["el." + s], J["wr." + s]
        for k, (dx, dy, dz, ln) in enumerate(((0.6, -0.4, 1.0, 5.0), (1.0, 0.4, 0.5, 3.8), (0.8, -1.0, 0.3, 3.4))):
            m.horn(sh + V3(sx * 1.6, -0.5 + k * 0.6, 1.8 - k * 0.6), (sx * dx, dy, dz), ln, 0.75, "spike",
                   "upperarm." + s, curl=(-8, 0, 0), segs=6, steps=4)
        for k in range(3):
            p = el.lerp(wr, 0.2 + 0.3 * k)
            m.horn(p + V3(sx * 1.2, -1.0, 0.4), (sx * 0.7, -1.0, 0.25), 2.8 - 0.5 * k, 0.5, "spike", "forearm." + s,
                   segs=5, steps=3)
        claws(m, J, s, "claw", n=4, length=3.0, r=0.45, spread=16)
        m.ellip(J["wr." + s].lerp(J["tip." + s], 0.7), (2.0, 2.0, 2.0), "glow", "hand." + s, segs=(12, 8), name="palm")
        hk, an, to = J["hock." + s], J["ankle." + s], J["toe." + s]
        m.horn(hk + V3(0, -1.0, 0.0), (0, -1, 0.4), 2.2, 0.55, "spike", "meta." + s, segs=5, steps=3)
        for k in (-1, 0, 1):
            m.horn(to + V3(k * 1.0, -0.6, 0.6), (k * 0.25, 1, -0.4), 2.0, 0.4, "claw", "foot." + s, segs=5, steps=3)
    for k in range(5):
        z = C.z + 4 - k * 3.2
        p = V3(0, C.y - 5.5 + k * 0.4, z)
        bone = "chest" if k < 2 else ("spine" if k < 4 else "hips")
        m.horn(p, (0, -1, 0.5), 4.0 - k * 0.4, 0.8, "spike", bone, curl=(-8, 0, 0), segs=6, steps=4)
    finish(m, tex)
    biped_clips(m, dict(attack="throw", death="back", digi=True, jaw=True, stride=30, knee=44, armswing=26, leglen=24,
                        lean_walk=8, twist=8, walk_frames=50))
    return m


# ================================================================== 6 pinky-role: the Maw
@builder
def pinky(name, tex):
    H = 56.0
    m = Model(name, H, 30)
    m.paint("flesh", (0.88, 0.50, 0.50), color2=(0.56, 0.16, 0.20), pattern="veins", pscale=3.5, pamount=0.8,
            wear=0.0, grime=0.7, grime_color=(0.30, 0.05, 0.08), pointy=0.8)
    m.paint("plate", (0.55, 0.20, 0.22), wear=0.5, wear_color=(0.85, 0.65, 0.55), seams=(3.0, 0.4), grime=0.6)
    m.paint("tooth", (0.94, 0.90, 0.76), wear=0.0, grime=0.8, grime_color=(0.45, 0.25, 0.12))
    m.paint("mouth", (0.32, 0.03, 0.05), color2=(0.6, 0.1, 0.12), pattern="mottle", pscale=1.5, wear=0, grime=0.3)
    m.paint("eye", (1.0, 0.9, 0.25), glow=True)
    m.paint("claw", (0.16, 0.10, 0.09), wear=0.3)
    J = {}
    J["root"], J["pelvis"], J["waist"] = V3(0, 0, 0), V3(0, -8, 25), V3(0, -2, 30)
    J["chest"], J["neck"], J["headb"], J["headtop"] = V3(0, 6, 33), V3(0, 12, 33), V3(0, 15, 32), V3(0, 27, 34)
    for s, sx in (("R", 1), ("L", -1)):
        J["sh." + s], J["el." + s] = V3(sx * 10.5, 9, 30), V3(sx * 12, 13, 24)
        J["wr." + s], J["tip." + s] = V3(sx * 11, 17, 21.5), V3(sx * 10, 19.5, 20)
        J["hip." + s], J["knee." + s] = V3(sx * 9.5, -8, 24), V3(sx * 12, -1, 16)
        J["hock." + s], J["ankle." + s], J["toe." + s] = V3(sx * 12, -9, 8), V3(sx * 12, -6.5, 2), V3(sx * 12, 2, 0)
    hinge, chin = V3(0, 14, 26), V3(0, 31, 18)
    add_biped_bones(m, J, digi=True, jaw=(hinge, chin))
    organic_body(m, J, "flesh", dict(pel=10.0, waist=13.0, chest=14.0, uc=12.5, neck=10.5, hb=9.5, sh=4.2, el=3.0,
                                     wr=2.4, tip=2.0, hip=8.0, knee=6.2, hock=4.2, ankle=3.4, toe=3.0), digi=True,
                 displace=0.9, dscale=6.0)
    for s, sx in (("R", 1), ("L", -1)):
        muscle(m, V3(sx * 9.5, -2, 28), (6.0, 8.0, 6.5), "flesh")
        muscle(m, J["knee." + s] + V3(sx * 0.5, -3.5, -3.0), (4.2, 4.6, 5.5), "flesh")
        claws(m, J, s, "claw", n=3, length=2.2, r=0.5, spread=26)
        to = J["toe." + s]
        for k in (-1, 0, 1):
            m.horn(to + V3(k * 1.6, -0.5, 1.0), (k * 0.3, 1, -0.3), 2.6, 0.7, "claw", "foot." + s, segs=6, steps=3)
    # skull + upper jaw
    m.ellip(V3(0, 20, 32), (11.5, 10.5, 8.5), "flesh", "head", segs=(22, 14), noise=0.05, name="skull")
    m.box(V3(0, 23.5, 37.5), (13.0, 9.0, 3.2), "plate", "head", bevel=0.4, taper=(0.7, 0.8), name="crown")
    m.box(V3(0, 29.0, 29.0), (16.0, 6.0, 5.0), "flesh", "head", bevel=0.45, taper=(0.9, 0.85), name="snout")
    m.ellip(V3(0, 24, 24.5), (11.5, 8.5, 4.4), "mouth", "head", segs=(18, 10), name="maw")
    m.sym(m.ellip, V3(6.0, 27.8, 33.4), (1.4, 0.9, 0.9), "eye", "head", segs=(10, 6))
    m.sym(m.box, V3(6.0, 27.0, 35.0), (4.0, 2.4, 1.6), "plate", "head", rot=(0, -15, 0), bevel=0.4)
    for i in range(9):
        a = math.radians(-70 + i * 17.5)
        p = V3(10.5 * math.sin(a), 22 + 9.5 * math.cos(a), 26.5)
        ln = 3.8 if i in (1, 7) else 2.6
        m.horn(p, (0.1 * math.sin(a), 0.15, -1), ln, 0.75 if i in (1, 7) else 0.6, "tooth", "head", segs=6, steps=3)
    # lower jaw: the big underbite
    m.ellip(V3(0, 23.5, 20.5), (12.8, 11.0, 4.8), "flesh", "jaw", segs=(20, 12), noise=0.05, name="jaw")
    m.box(V3(0, 31.0, 18.0), (12.0, 3.0, 3.0), "plate", "jaw", bevel=0.4, name="chin")
    for i in range(9):
        a = math.radians(-72 + i * 18)
        p = V3(11.5 * math.sin(a), 23.5 + 9.8 * math.cos(a), 23.0)
        big = i in (0, 8)
        m.horn(p, (0.05 * math.sin(a), 0.1, 1), 6.5 if big else 3.6, 1.0 if big else 0.7, "tooth", "jaw",
               curl=(6 if big else 0, 0, 0), segs=6, steps=4)
    # armoured spine ridge + exposed ribs
    for k in range(7):
        t = k / 6.0
        p = V3(0, -9 + 22 * t, 34 + 9 * math.sin(math.pi * (0.25 + 0.6 * t)))
        bone = "hips" if k < 2 else ("spine" if k < 4 else "chest")
        m.box(p, (7.0 - abs(k - 3) * 0.6, 4.2, 2.6), "plate", bone, rot=(-20 + 40 * t, 0, 0), bevel=0.4,
              taper=(0.7, 0.8), name="ridge")
        m.horn(p + V3(0, -0.5, 1.0), (0, -0.6, 1), 3.5, 0.8, "tooth", bone, curl=(-10, 0, 0), segs=6, steps=3)
    for k in range(3):
        for s, sx in (("R", 1), ("L", -1)):
            y = -2 + k * 4.5
            pts = [V3(sx * 2.5, y, 44), V3(sx * 10.5, y + 0.5, 39), V3(sx * 13.5, y + 1.0, 31)]
            m.tube(pts, [0.8, 0.9, 0.6], "tooth", "spine" if k < 1 else "chest", segs=6, name="rib")
    finish(m, tex)
    biped_clips(m, dict(attack="bite", death="side", digi=True, jaw=True, stride=26, knee=40, armswing=10, leglen=22,
                        lean_walk=3, twist=7, walk_frames=48, heavy=1.4))
    return m


# ================================================================== 7 caco-role: the Gloom Orb
@builder
def caco(name, tex):
    H, c = 56.0, 28.0
    m = Model(name, H, 31)
    m.paint("shell", (0.55, 0.09, 0.11), color2=(0.36, 0.04, 0.07), pattern="mottle", pscale=6, wear=0.7,
            wear_color=(0.85, 0.42, 0.35), grime=0.7, grime_color=(0.12, 0.01, 0.02), seams=(5.0, 0.45))
    m.paint("flesh", (0.38, 0.05, 0.08), color2=(0.6, 0.14, 0.18), pattern="veins", pscale=3, wear=0.0, grime=0.6,
            pointy=0.8)
    m.paint("spike", (0.90, 0.84, 0.70), wear=0.0, grime=0.8, grime_color=(0.4, 0.2, 0.1))
    m.paint("eye", (0.70, 1.0, 0.25), glow=True)
    m.paint("mouth", (0.42, 0.03, 0.05), color2=(0.75, 0.18, 0.12), pattern="mottle", pscale=2, wear=0.0, grime=0.3)
    jaw = ((0, -8, c - 6), (0, 16, c - 10))
    tents = [("tent1", (-7, -4, c - 16), (-8, -7, 3)), ("tent2", (7, -4, c - 16), (8, -7, 3)),
             ("tent3", (0, -9, c - 15), (0, -13, 5))]
    add_flyer_bones(m, c, 20, jaw=jaw, extra=[(n, h, t, "body") for n, h, t in tents])
    m.ellip((0, 0, c), (19.5, 19.0, 18.0), "flesh", "body", segs=(32, 20), noise=0.03, name="core")
    # trilobite carapace: overlapping curved bands front to back
    R = 19.6
    for i in range(8):
        a = math.radians(-80 + i * 17)
        cy, cz = R * math.sin(a), c + 1.0 + R * math.cos(a) * 0.97
        w = 18.5 * (1 - 0.22 * abs(i - 3.5) / 3.5)
        m.ellip((0, cy, cz), (w, 3.6, 2.0), "shell", "body", rot=(-math.degrees(a), 0, 0), segs=(28, 10), name="band")
    m.sym(m.ellip, (15.5, 0, c + 1), (5.5, 15.0, 13.0), "shell", "body", rot=(0, 18, 0), segs=(20, 12))
    # ridge spines
    for i in range(6):
        a = math.radians(-35 + i * 22)
        p = (0, (R + 2.5) * math.sin(a), c + (R + 1.5) * math.cos(a))
        m.horn(p, (0, math.sin(a) * 0.4 - 0.4, 1.0), 6.0 - abs(i - 2.5) * 0.7, 1.3, "spike", "body", curl=(-10, 0, 0),
               segs=7, steps=4)
    for s in (1, -1):
        for k in range(3):
            a = math.radians(40 + k * 22)
            p = (s * 19.0 * math.sin(a) * 1.05, -2 + k * 4, c + 6 - k * 3)
            m.horn(p, (s * 1.0, -0.2, 0.5), 4.0, 0.9, "spike", "body", curl=(0, -s * 10, 0), segs=6, steps=3)
    # three eyes in armoured sockets
    m.ellip((0, 16.4, c + 6.5), (3.6, 2.6, 3.4), "shell", "body", segs=(14, 10))
    m.ellip((0, 18.2, c + 6.5), (2.6, 1.6, 2.6), "eye", "body", segs=(14, 10), name="eye_c")
    m.sym(m.ellip, (7.2, 15.2, c + 7.2), (2.2, 2.0, 2.0), "shell", "body", segs=(12, 8))
    m.sym(m.ellip, (7.3, 16.6, c + 7.3), (1.5, 1.1, 1.5), "eye", "body", segs=(12, 8))
    # mouth: dark cavity, upper teeth, hinged lower shell with teeth
    m.ellip((0, 12.5, c - 5), (13.5, 7.5, 5.5), "mouth", "body", segs=(20, 10), name="maw")
    for i in range(11):
        a = math.radians(-60 + i * 12)
        p = (13.0 * math.sin(a), 6 + 12.5 * math.cos(a), c - 2.5)
        m.horn(p, (0, 0.2, -1), 3.4 if i % 2 == 0 else 2.4, 0.7, "spike", "body", segs=5, steps=3)
    m.ellip((0, 5.5, c - 11), (16.5, 13.5, 6.5), "shell", "jaw", segs=(24, 12), name="jaw")
    for i in range(9):
        a = math.radians(-55 + i * 13.75)
        p = (14.0 * math.sin(a), 5.5 + 12.5 * math.cos(a), c - 7)
        m.horn(p, (0, 0.1, 1), 3.0, 0.65, "spike", "jaw", segs=5, steps=3)
    for n, h, t in tents:
        h, t = Vector(h), Vector(t)
        pts = [h, h.lerp(t, 0.35) + Vector((0, 1.5, 0)), h.lerp(t, 0.7) + Vector((0, -1.0, 0)), t]
        m.tube(pts, [2.2, 1.6, 1.0, 0.0], "flesh", n, segs=8, name="tendril")
    finish(m, tex)
    flyer_clips(m, dict(arms=[n for n, _, _ in tents], centre=c, attack="spit", bob=1.8))
    return m


# ================================================================== 8 lost-soul role: the Cinder Skull
@builder
def lostsoul(name, tex):
    H, c = 30.0, 14.0
    m = Model(name, H, 16, tex=512)
    m.paint("bone", (0.90, 0.85, 0.74), wear=0.0, grime=0.9, grime_color=(0.25, 0.10, 0.05), seams=(2.5, 0.35),
            spots=((0.15, 0.06, 0.03), 2.0, 0.7))
    m.paint("eye", (1.0, 0.75, 0.15), glow=True)
    m.paint("flame", (1.0, 0.40, 0.04), glow=True)
    m.paint("core", (1.0, 0.82, 0.30), glow=True)
    m.paint("dark", (0.10, 0.03, 0.02), wear=0.0, grime=0.0)
    jaw = ((0, 1.0, c - 2.5), (0, 7.0, c - 5.0))
    fl = [("flame1", (0, -5, c + 3), (0, -14, c + 9)), ("flame2", (3.5, -4, c + 1), (6, -12, c + 4)),
          ("flame3", (-3.5, -4, c + 1), (-6, -12, c + 4))]
    add_flyer_bones(m, c, 8, jaw=jaw, extra=[(n, h, t, "body") for n, h, t in fl])
    m.ellip((0, -1.0, c + 2.0), (5.6, 7.0, 6.0), "bone", "body", segs=(32, 20), name="cranium")
    m.box((0, 4.6, c + 3.6), (9.0, 2.0, 2.0), "bone", "body", bevel=0.45, name="brow")
    m.ellip((0, 3.6, c - 1.2), (4.4, 4.0, 3.0), "bone", "body", segs=(16, 10), name="maxilla")
    m.sym(m.ellip, (2.2, 5.3, c + 1.9), (1.6, 0.8, 1.3), "dark", "body", segs=(10, 6))
    m.sym(m.ellip, (2.2, 5.8, c + 1.9), (1.0, 0.5, 0.8), "eye", "body", segs=(10, 6))
    m.ellip((0, 6.9, c - 0.2), (0.9, 0.6, 1.0), "dark", "body", segs=(8, 6), name="nose")
    for i in range(6):
        x = (i - 2.5) * 1.15
        m.box((x, 6.8 - abs(x) * 0.35, c - 3.6), (0.95, 0.9, 1.6), "bone", "body", bevel=0.3)
    m.ellip((0, 4.2, c - 5.6), (3.8, 3.8, 1.6), "bone", "jaw", segs=(14, 8), name="mandible")
    for i in range(5):
        x = (i - 2) * 1.2
        m.box((x, 7.0 - abs(x) * 0.35, c - 4.4), (0.95, 0.9, 1.4), "bone", "jaw", bevel=0.3)
    m.sym(m.horn, (3.4, -1.0, c + 5.5), (0.6, -0.7, 0.6), 6.0, 1.1, "bone", "body", curl=(-16, 0, 0))
    m.sym(m.horn, (4.6, 0.5, c + 2.5), (1.0, -0.4, 0.1), 3.5, 0.7, "bone", "body", curl=(-10, 0, 0))
    for n, h, t in fl:
        h, t = Vector(h), Vector(t)
        d = (t - h)
        main = n == "flame1"
        pts = [h, h + d * 0.4 + Vector((0, 0, 1.5)), h + d * 0.75, h + d * 1.25 + Vector((0, 0, 2.5))]
        m.tube(pts, [4.6 if main else 3.2, 3.6 if main else 2.4, 2.0, 0.0], "flame", n, segs=14, name="flame")
        m.tube([h + Vector((0, 0.6, 0)), h + d * 0.5, h + d * 0.9], [2.8 if main else 2.0, 1.6, 0.0], "core", n, segs=8)
    for k in range(5):
        a = 2 * math.pi * k / 5
        p = Vector((4.2 * math.cos(a), -6.5, c + 2 + 3.6 * math.sin(a)))
        m.tube([p, p + Vector((math.cos(a) * 1.5, -5, 2 + math.sin(a))), p + Vector((math.cos(a) * 2, -9, 4))],
               [1.5, 0.9, 0.0], "flame", "flame1" if k < 2 else ("flame2" if k < 4 else "flame3"), segs=6)
    finish(m, tex, max_tris=8000)
    flyer_clips(m, dict(arms=[n for n, _, _ in fl], centre=c, attack="charge", bob=1.2, lunge=10.0))
    return m


# ================================================================== 9 revenant-role: the Ossuary Gunner
@builder
def revenant(name, tex):
    H = 64.0
    m = Model(name, H, 20)
    m.paint("bone", (0.88, 0.84, 0.72), wear=0.0, grime=0.9, grime_color=(0.30, 0.18, 0.10), seams=(2.0, 0.3))
    m.paint("sinew", (0.55, 0.12, 0.12), color2=(0.30, 0.05, 0.06), pattern="stripes", pscale=0.8, wear=0.0, grime=0.5)
    m.paint("metal", (0.20, 0.21, 0.24), wear=0.9, wear_color=(0.65, 0.65, 0.70), seams=(3.0, 0.5))
    m.paint("pod", (0.30, 0.32, 0.30), color2=(0.85, 0.65, 0.08), pattern="stripes", pscale=1.4, pamount=0.5,
            wear=0.8, wear_color=(0.7, 0.7, 0.7))
    m.paint("tip", (0.80, 0.10, 0.06), wear=0.4)
    m.paint("eye", (1.0, 0.15, 0.05), glow=True)
    J = biped_joints(H, hip=0.5, shw=0.15, chest=0.73, shoulder=0.80, neck=0.85, headb=0.86, headtop=0.97, hunch=0.05,
                     stance=0.01)
    hang_arms(J, H, ua=0.21, fa=0.2, hand=0.08, out=12, fwd=10, elbow=30)
    hb = J["headb"]
    hc = hb + V3(0, 0.8, 3.6)
    hinge, chin = hb + V3(0, 1.6, 1.2), hb + V3(0, 4.8, -0.6)
    add_biped_bones(m, J, jaw=(hinge, chin))
    organic_body(m, J, "sinew", dict(pel=2.6, waist=1.7, chest=2.6, uc=2.8, neck=1.4, hb=1.5, sh=1.9, el=1.3, wr=1.1,
                                     tip=1.0, hip=2.0, knee=1.6, ankle=1.3, toe=1.0), displace=0.2, dscale=2.0)
    P, W, C, N = J["pelvis"], J["waist"], J["chest"], J["neck"]
    # bones over the sinew: limbs, spine, pelvis, ribs
    for s, sx in (("R", 1), ("L", -1)):
        for a, b, bone, r in (("sh", "el", "upperarm", 1.0), ("el", "wr", "forearm", 0.85),
                              ("hip", "knee", "thigh", 1.25), ("knee", "ankle", "shin", 1.05)):
            pa, pb = J[a + "." + s], J[b + "." + s]
            m.tube([pa, pa.lerp(pb, 0.5), pb], [r * 1.25, r, r * 1.25], "bone", bone + "." + s, segs=8, name="limb")
        m.ellip(J["el." + s], (1.9, 1.9, 1.9), "metal", "forearm." + s, segs=(10, 8))
        m.ellip(J["knee." + s] + V3(0, 0.6, 0), (2.2, 2.2, 2.2), "metal", "shin." + s, segs=(10, 8))
        m.box(J["ankle." + s] + V3(0, 2.6, -1.4), (3.2, 7.5, 2.0), "metal", "foot." + s, bevel=0.35, taper=(0.8, 0.8))
        claws(m, J, s, "bone", n=3, length=3.4, r=0.4, spread=18)
        m.tube([J["sh." + s] + V3(0, 0.6, 0.6), J["el." + s] + V3(0, 0.6, 0)], [0.7, 0.5], "sinew", "upperarm." + s, segs=6)
    m.ellip(P + V3(0, 0.2, -0.4), (5.6, 3.6, 3.2), "bone", "hips", segs=(16, 10), name="pelvis")
    for k in range(7):
        z = P.z + 2 + k * (N.z - P.z - 2) / 7
        y = P.y + (N.y - P.y) * k / 7 - 1.2
        m.box((0, y - 0.6, z), (1.9, 2.4, 1.4), "bone", "spine" if k < 3 else "chest", bevel=0.35, name="vertebra")
    for k in range(5):
        z = C.z + 3.5 - k * 2.0
        w = 6.0 - abs(k - 1.5) * 0.5
        for sx in (1, -1):
            pts = [V3(0.6 * sx, C.y - 3.6, z + 0.6), V3(sx * w, C.y - 1.8, z), V3(sx * w * 0.95, C.y + 2.8, z - 0.8),
                   V3(sx * 1.2, C.y + 4.6, z - 1.6)]
            m.tube(pts, [0.55, 0.6, 0.55, 0.45], "bone", "chest" if k < 3 else "spine", segs=6, name="rib")
    m.box(C + V3(0, 4.6, 0.6), (1.8, 0.9, 8.0), "bone", "chest", bevel=0.4, name="sternum")
    # skull
    m.ellip(hc, (3.3, 4.0, 3.9), "bone", "head", segs=(18, 12), name="skull")
    m.box(hc + V3(0, 2.9, 0.9), (6.2, 1.6, 1.4), "bone", "head", bevel=0.45, name="brow")
    m.sym(m.ellip, hc + V3(1.3, 3.4, 0.0), (1.0, 0.7, 0.9), "eye", "head", segs=(10, 6))
    m.ellip(hc + V3(0, 2.6, -2.0), (2.4, 1.8, 1.4), "bone", "head", segs=(12, 8), name="maxilla")
    m.ellip(hinge.lerp(chin, 0.6) + V3(0, 0.4, -0.6), (2.3, 2.0, 1.0), "bone", "jaw", segs=(12, 8), name="mandible")
    for i in range(5):
        x = (i - 2) * 0.9
        m.box(hc + V3(x, 4.2 - abs(x) * 0.3, -2.9), (0.75, 0.7, 1.0), "bone", "head", bevel=0.3)
    # harness + shoulder missile pods
    m.box(C + V3(0, -3.8, 1.0), (9.0, 2.6, 8.0), "metal", "chest", bevel=0.3, name="backplate")
    m.sym(m.box, C + V3(3.0, 0.2, 2.8), (1.6, 9.0, 1.2), "metal", "chest", rot=(0, -20, 0), bevel=0.3)
    for sx in (1, -1):
        pc = C + V3(sx * 7.2, -4.2, 9.0)
        m.box(pc, (6.4, 8.0, 5.6), "pod", "chest", rot=(-18, 0, 0), bevel=0.25, name="pod")
        m.box(pc + V3(-sx * 2.4, 1.0, -3.6), (1.6, 2.4, 4.0), "metal", "chest", bevel=0.3)
        for k in range(4):
            q = pc + V3(((k % 2) - 0.5) * 2.6, 3.9, 0.4 + (k // 2) * 2.2)
            m.cyl(tuple(q), tuple(q + V3(0, 1.6, 0.5)), 0.8, "tip", "chest", segs=8)
    finish(m, tex)
    biped_clips(m, dict(attack="punch", death="back", jaw=True, stride=30, knee=50, armswing=26, leglen=32,
                        lean_walk=6, twist=10, walk_frames=56))
    return m


# ================================================================== 10/11 baron-role + knight-role: Hell Regent
def regent(name, tex, palette):
    H = 64.0
    m = Model(name, H, 24)
    hide, hide2, fur, glow = palette
    m.paint("hide", hide, color2=hide2, pattern="veins", pscale=3.0, pamount=0.7, wear=0.0, grime=0.7,
            grime_color=(0.15, 0.03, 0.02), zband=(27.0, 32.0, fur), pointy=0.9)
    m.paint("fur", fur, color2=tuple(x * 0.6 for x in fur), pattern="stripes", pscale=0.6, wear=0.0, grime=0.6,
            pointy=0.7)
    m.paint("horn", (0.88, 0.82, 0.66), color2=(0.45, 0.35, 0.25), pattern="stripes", pscale=1.2, pamount=0.5,
            wear=0.0, grime=0.8, grime_color=(0.3, 0.15, 0.05))
    m.paint("hoof", (0.10, 0.08, 0.07), wear=0.5, wear_color=(0.35, 0.3, 0.25))
    m.paint("glow", glow, glow=True)
    m.paint("mouth", (0.25, 0.03, 0.03), wear=0, grime=0.3)
    J = biped_joints(H, hip=0.5, digi=True, shw=0.205, chest=0.74, shoulder=0.80, neck=0.84, headb=0.85, headtop=0.965,
                     hunch=0.04, stance=0.025)
    hang_arms(J, H, ua=0.19, fa=0.18, hand=0.075, out=18, fwd=10, elbow=26)
    hb = J["headb"]
    hc = hb + V3(0, 1.2, 3.8)
    hinge, chin = hb + V3(0, 1.8, 1.6), hb + V3(0, 6.8, -0.6)
    add_biped_bones(m, J, digi=True, jaw=(hinge, chin))
    organic_body(m, J, "hide", dict(pel=7.6, waist=7.2, chest=10.0, uc=10.2, neck=4.8, hb=4.0, sh=5.2, el=3.8, wr=3.2,
                                    tip=2.8, hip=6.4, knee=5.0, hock=3.2, ankle=2.4, toe=2.2), digi=True, displace=0.7)
    C, W = J["chest"], J["waist"]
    for s, sx in (("R", 1), ("L", -1)):
        sh, el, wr = J["sh." + s], J["el." + s], J["wr." + s]
        muscle(m, C + V3(sx * 4.6, 6.0, 0.6), (5.0, 3.0, 4.4), "hide")
        muscle(m, sh + V3(sx * 0.8, 0, 0.6), (5.0, 5.0, 4.6), "hide")
        muscle(m, sh.lerp(el, 0.5) + V3(0, 1.2, 0), (3.6, 3.6, 5.0), "hide")
        muscle(m, el.lerp(wr, 0.35), (3.4, 3.4, 4.4), "hide")
        muscle(m, C + V3(sx * 4.0, -1.0, 6.0), (4.6, 4.0, 3.0), "hide")
        muscle(m, W + V3(sx * 2.4, 5.4, 0.0), (2.6, 2.0, 2.6), "hide")
        muscle(m, J["knee." + s] + V3(0, -3.0, -4.0), (3.8, 4.4, 5.6), "fur")
        muscle(m, J["hip." + s] + V3(sx * 1.0, 1.5, -3.5), (5.6, 5.8, 6.4), "fur")
        claws(m, J, s, "hoof", n=4, length=2.6, r=0.55, spread=18)
        m.ellip(J["wr." + s].lerp(J["tip." + s], 0.55), (3.6, 3.6, 3.4) if s == "R" else (2.6, 2.6, 2.5), "glow",
                "hand." + s, segs=(14, 10), name="handglow")
        an, to, hk = J["ankle." + s], J["toe." + s], J["hock." + s]
        m.box(to + V3(0, -1.5, 1.3), (4.6, 5.0, 3.0), "hoof", "foot." + s, bevel=0.35, taper=(0.8, 0.7), name="hoof")
        for k in range(4):
            m.horn(hk + V3(sx * (0.8 + 0.3 * k), -1.2, 3.0 - k * 1.2), (sx * 0.5, -1, -0.3), 3.0, 0.7, "fur",
                   "shin." + s, segs=5, steps=3)
        for k in range(3):
            m.horn(sh + V3(sx * (1.5 + k), -1.0 + k * 1.2, 3.4 - k), (sx * 0.4, -0.3, 1), 4.5 - k, 1.1, "horn",
                   "upperarm." + s, curl=(-6, 0, 0), segs=6, steps=4)
    m.ellip(hc, (4.2, 5.0, 4.2), "hide", "head", segs=(18, 12), noise=0.05, name="skull")
    m.box(hc + V3(0, 3.4, 1.2), (7.6, 2.6, 2.0), "hide", "head", bevel=0.45, rot=(-14, 0, 0), name="brow")
    m.sym(m.ellip, hc + V3(1.7, 4.4, 0.3), (1.0, 0.5, 0.55), "glow", "head", segs=(10, 6))
    m.ellip(hc + V3(0, 3.6, -1.8), (3.0, 2.6, 1.6), "mouth", "head", segs=(12, 8), name="maw")
    m.ellip(hinge.lerp(chin, 0.62) + V3(0, 0.2, -0.6), (3.2, 3.2, 1.5), "hide", "jaw", segs=(12, 8), name="jaw")
    for i in range(5):
        x = (i - 2) * 1.2
        m.horn(hc + V3(x, 5.0 - abs(x) * 0.3, -1.8), (0, 0.2, -1), 1.4, 0.4, "horn", "head", segs=5, steps=2)
    m.sym(m.horn, hc + V3(2.8, -0.4, 2.4), (1.0, -0.2, 0.5), 14.0, 1.7, "horn", "head", curl=(0, -16, 8), segs=10,
          steps=8, name="horn")
    finish(m, tex)
    biped_clips(m, dict(attack="throw", death="back", digi=True, jaw=True, stride=26, knee=42, armswing=20, leglen=30,
                        lean_walk=5, twist=7, walk_frames=64))
    return m


@builder
def baron(name, tex):
    return regent(name, tex, ((0.66, 0.14, 0.11), (0.40, 0.05, 0.05), (0.64, 0.50, 0.32), (0.35, 1.0, 0.25)))


@builder
def hellknight(name, tex):
    return regent(name, tex, ((0.60, 0.44, 0.28), (0.36, 0.22, 0.12), (0.30, 0.20, 0.12), (0.35, 1.0, 0.25)))


# ================================================================== 12 arch-vile role: the Pyre Priest
@builder
def archvile(name, tex):
    H = 60.0
    m = Model(name, H, 20)
    m.paint("skin", (0.86, 0.84, 0.76), color2=(0.45, 0.30, 0.20), pattern="cracks", pscale=2.0, pamount=0.8,
            wear=0.0, grime=0.8, grime_color=(0.25, 0.12, 0.06), spots=((0.40, 0.15, 0.06), 3.0, 0.6), pointy=0.8)
    m.paint("cloth", (0.45, 0.07, 0.05), color2=(0.22, 0.03, 0.02), pattern="stripes", pscale=0.8, wear=0.2, grime=0.8)
    m.paint("bone", (0.70, 0.62, 0.48), wear=0.0, grime=0.9, grime_color=(0.2, 0.1, 0.05))
    m.paint("ember", (1.0, 0.50, 0.08), glow=True)
    m.paint("eye", (1.0, 0.85, 0.3), glow=True)
    J = biped_joints(H, hip=0.53, shw=0.15, chest=0.76, shoulder=0.81, neck=0.85, headb=0.86, headtop=0.985, hunch=0.06,
                     stance=0.015, knee=0.29)
    hang_arms(J, H, ua=0.24, fa=0.24, hand=0.1, out=18, fwd=10, elbow=26)
    hb = J["headb"]
    hc = hb + V3(0, 1.0, 4.2)
    hinge, chin = hb + V3(0, 1.2, 1.4), hb + V3(0, 4.6, -1.4)
    add_biped_bones(m, J, jaw=(hinge, chin))
    organic_body(m, J, "skin", dict(pel=4.4, waist=3.2, chest=5.4, uc=5.6, neck=2.0, hb=2.2, sh=2.4, el=1.6, wr=1.4,
                                    tip=1.2, hip=3.0, knee=2.2, ankle=1.6, toe=1.3), displace=0.5, dscale=3.0)
    P, C = J["pelvis"], J["chest"]
    for k in range(4):
        z = C.z + 1.5 - k * 2.0
        for sx in (1, -1):
            m.tube([V3(sx * 1.0, C.y + 4.8, z), V3(sx * 4.6, C.y + 3.0, z - 0.5), V3(sx * 5.4, C.y - 0.5, z - 0.8)],
                   [0.5, 0.55, 0.4], "skin", "chest", segs=6, name="rib")
    m.ellip(hc, (2.9, 3.4, 4.6), "skin", "head", segs=(16, 12), noise=0.05, name="skull")
    m.box(hc + V3(0, 2.6, 1.2), (5.0, 1.6, 1.4), "skin", "head", bevel=0.45, name="brow")
    m.sym(m.ellip, hc + V3(1.15, 3.0, 0.5), (0.8, 0.4, 0.5), "eye", "head", segs=(8, 6))
    m.ellip(hinge.lerp(chin, 0.6), (2.0, 2.0, 1.1), "skin", "jaw", segs=(10, 8), name="jaw")
    for k in range(7):
        a = math.radians(-70 + k * 23.3)
        p = hc + V3(2.8 * math.sin(a), 3.0 * math.cos(a) - 0.6, 3.0)
        m.horn(p, (math.sin(a) * 0.5, math.cos(a) * 0.3, 1), 5.0 if k == 3 else 3.6, 0.6, "bone", "head",
               curl=(0, 0, 0), segs=6, steps=3, name="crown")
    m.horn(hc + V3(0, 1.0, 7.6), (0, 0, 1), 3.0, 1.0, "ember", "head", segs=8, steps=3, name="crownflame")
    # tattered mantle + loincloth
    for k in range(7):
        a = math.radians(-150 + k * 50)
        p = C + V3(6.0 * math.cos(math.radians(k * 51.4)), 4.5 * math.sin(math.radians(k * 51.4)), 5.0)
        m.box(p, (3.4, 0.5, 6.0), "cloth", "chest", rot=(0, 0, math.degrees(math.atan2(p.x, p.y - C.y)) + 90), bevel=0.2,
              taper=(0.5, 1), name="mantle")
    for k, (x, y, rx) in enumerate(((0, 4.0, 6), (-2.8, 3.4, 4), (2.8, 3.4, 4), (0, -4.0, -6), (3.6, -2.5, -4),
                                     (-3.6, -2.5, -4))):
        m.box(P + V3(x, y, -6.5 - (k % 3) * 1.5), (3.2, 0.5, 13.0 - (k % 3) * 3), "cloth", "hips", rot=(rx, 0, 0),
              bevel=0.2, taper=(0.4, 1), name="tatter")
    m.box(P + V3(0, 0.0, 0.2), (9.6, 7.6, 2.2), "bone", "hips", bevel=0.4, name="girdle")
    for s, sx in (("R", 1), ("L", -1)):
        wr, tip = J["wr." + s], J["tip." + s]
        d = (tip - wr).normalized()
        for k in range(4):
            off = V3(sx * (k - 1.5) * 0.7, 0, 0)
            m.horn(tip - d * 1.0 + off, d + V3(sx * (k - 1.5) * 0.15, 0.2, 0), 5.5 - abs(k - 1.5), 0.45, "skin",
                   "hand." + s, curl=(-10, 0, 0), segs=5, steps=4, name="finger")
        m.ellip(wr.lerp(tip, 0.5), (1.7, 1.7, 1.7), "ember", "hand." + s, segs=(10, 8))
        m.horn(wr.lerp(tip, 0.4) + V3(0, 0.5, 1.0), (0, 0.2, 1), 5.0, 1.4, "ember", "hand." + s, curl=(-6, 0, 0),
               segs=8, steps=4, name="handflame")
        m.box(J["ankle." + s] + V3(0, 1.8, -1.2), (2.6, 7.0, 1.6), "skin", "foot." + s, bevel=0.4, taper=(0.7, 0.8))
        m.box(J["el." + s].lerp(J["wr." + s], 0.5), (3.2, 3.2, 5.0), "cloth", "forearm." + s, bevel=0.3)
    finish(m, tex)
    biped_clips(m, dict(attack="cast", death="front", jaw=True, stride=24, knee=40, armswing=18, leglen=32,
                        lean_walk=6, twist=6, walk_frames=60))
    return m


# ================================================================== 13 mancubus-role: the Furnace Glutton
@builder
def mancubus(name, tex):
    H = 64.0
    m = Model(name, H, 48)
    m.paint("flesh", (0.70, 0.55, 0.40), color2=(0.50, 0.33, 0.22), pattern="mottle", pscale=5, wear=0.0, grime=0.8,
            grime_color=(0.22, 0.10, 0.05), seams=(6.0, 0.25), pointy=0.8)
    m.paint("metal", (0.24, 0.25, 0.27), wear=0.9, wear_color=(0.70, 0.70, 0.72), seams=(3.0, 0.5), grime=0.7)
    m.paint("strap", (0.22, 0.14, 0.08), wear=0.4)
    m.paint("glow", (1.0, 0.48, 0.06), glow=True)
    m.paint("eye", (1.0, 0.85, 0.2), glow=True)
    m.paint("mouth", (0.30, 0.05, 0.04), wear=0.0, grime=0.3)
    m.paint("tooth", (0.85, 0.80, 0.62), wear=0.0, grime=0.7)
    J = {}
    J["root"], J["pelvis"], J["waist"] = V3(0, 0, 0), V3(0, 0, 21), V3(0, 2, 30)
    J["chest"], J["neck"], J["headb"], J["headtop"] = V3(0, 1, 41), V3(0, 2, 49), V3(0, 3, 50), V3(0, 5, 60)
    for s, sx in (("R", 1), ("L", -1)):
        J["sh." + s], J["el." + s] = V3(sx * 20, 0, 45), V3(sx * 27, 3, 35)
        J["wr." + s], J["tip." + s] = V3(sx * 26, 13, 32), V3(sx * 25, 22, 32)
        J["hip." + s], J["knee." + s] = V3(sx * 11, 0, 21), V3(sx * 13, 3, 11)
        J["ankle." + s], J["toe." + s] = V3(sx * 13, 0, 3.5), V3(sx * 13, 9, 0.5)
    hinge, chin = V3(0, 4, 51.5), V3(0, 10, 50)
    add_biped_bones(m, J, jaw=(hinge, chin))
    organic_body(m, J, "flesh", dict(pel=14.0, waist=20.0, chest=19.0, uc=16.0, neck=9.0, hb=7.0, sh=7.5, el=6.0, wr=5.6,
                                     tip=5.0, hip=9.5, knee=7.8, ankle=6.0, toe=4.6), displace=1.0, dscale=7.0)
    m.ellip(V3(0, 7, 28), (23.0, 18.0, 17.0), "flesh", None, segs=(26, 16), name="belly")
    m.sym(m.ellip, V3(13, 5, 38), (9.0, 9.0, 8.0), "flesh", None, segs=(16, 10))
    m.ellip(V3(0, 5, 55), (6.4, 6.6, 6.2), "flesh", "head", segs=(16, 12), name="head")
    m.box(V3(0, 10.2, 57), (8.0, 2.0, 1.8), "flesh", "head", bevel=0.45, name="brow")
    m.sym(m.ellip, V3(2.3, 10.6, 56.2), (1.0, 0.5, 0.6), "eye", "head", segs=(8, 6))
    m.ellip(V3(0, 9.6, 52.8), (3.6, 2.2, 1.6), "mouth", "head", segs=(12, 8))
    m.ellip(V3(0, 8, 50.0), (5.2, 4.0, 2.2), "flesh", "jaw", segs=(12, 8), name="jaw")
    m.sym(m.horn, V3(3.6, 10.2, 50.8), (0.3, 0.3, 1), 3.2, 0.8, "tooth", "jaw", curl=(-12, 0, 0), segs=6, steps=3)
    # harness, plates
    for k, (z, rx, ry) in enumerate(((22.0, 22.4, 17.6), (34.0, 21.6, 17.0))):
        m.ellip(V3(0, 7, z), (rx, ry, 1.3), "strap", None, segs=(28, 6), name="strap")
    m.ellip(V3(0, 4.5, 40.5), (19.0, 15.0, 1.3), "strap", None, segs=(28, 6), name="strap")
    m.box(V3(0, -12, 40), (20.0, 6.0, 14.0), "metal", "chest", bevel=0.25, name="tank")
    for sx in (1, -1):
        m.cyl((sx * 6, -15.5, 33), (sx * 6, -15.5, 50), 2.8, "metal", "chest", segs=12)
        m.tube([V3(sx * 6, -15.5, 49), V3(sx * 15, -10, 52), V3(sx * 22, -2, 46), V3(sx * 26, 3, 38)], [1.1] * 4,
               "strap", "chest", segs=6, name="hose")
    for s, sx in (("R", 1), ("L", -1)):
        sh, el, wr, tip = J["sh." + s], J["el." + s], J["wr." + s], J["tip." + s]
        m.box(sh + V3(sx * 1.5, 0, 6.0), (12.0, 13.0, 3.6), "metal", "upperarm." + s, rot=(0, sx * 22, 0),
              taper=(0.75, 0.8), bevel=0.3, name="pauldron")
        d = (tip - wr).normalized()
        base = el.lerp(wr, 0.35)
        m.cyl(tuple(base), tuple(tip + d * 6), 5.4, "metal", "forearm." + s, segs=16, name="cannon")
        for k in range(4):
            q = base.lerp(tip, 0.15 + k * 0.25)
            m.cyl(tuple(q), tuple(q + d * 1.4), 6.2, "strap" if k % 2 else "metal", "forearm." + s, segs=16)
        m.cyl(tuple(tip + d * 5.5), tuple(tip + d * 7.5), 4.4, "metal", "forearm." + s, segs=16, name="muzzle")
        m.cyl(tuple(tip + d * 7.0), tuple(tip + d * 7.7), 3.2, "glow", "forearm." + s, segs=16, name="muzzleglow")
        m.box(J["ankle." + s] + V3(0, 3.5, -2.0), (10.0, 13.0, 3.6), "flesh", "foot." + s, bevel=0.45, taper=(0.9, 0.8))
        for k in (-1, 0, 1):
            m.horn(J["toe." + s] + V3(k * 2.6, 1.0, 0.8), (0, 1, -0.2), 2.0, 1.0, "tooth", "foot." + s, segs=6, steps=2)
    finish(m, tex, max_tris=20000)
    biped_clips(m, dict(attack="cannons", death="back", jaw=True, stride=16, knee=30, armswing=6, leglen=20,
                        lean_walk=2, twist=10, walk_frames=72, gun=True, arm_fwd=95))
    return m


# ================================================================== 14 pain-elemental role: the Brood Sac
@builder
def painelemental(name, tex):
    H, c = 56.0, 28.0
    m = Model(name, H, 31)
    m.paint("flesh", (0.52, 0.34, 0.20), color2=(0.34, 0.20, 0.10), pattern="mottle", pscale=5, wear=0.0, grime=0.8,
            grime_color=(0.18, 0.08, 0.03))
    m.paint("vein", (0.45, 0.28, 0.16), color2=(0.70, 0.22, 0.10), pattern="veins", pscale=4, wear=0.0, grime=0.6,
            pointy=0.8)
    m.paint("glow", (1.0, 0.42, 0.05), glow=True)
    m.paint("eye", (1.0, 0.82, 0.15), glow=True)
    m.paint("tooth", (0.90, 0.85, 0.70), wear=0.0, grime=0.8)
    m.paint("claw", (0.14, 0.10, 0.08), wear=0.3)
    arms = [("arm.R", (17, 4, c - 2), (23, 12, c - 9)), ("arm.L", (-17, 4, c - 2), (-23, 12, c - 9))]
    add_flyer_bones(m, c, 22, jaw=((0, 2, c - 6), (0, 20, c - 12)), extra=[(n, h, t, "body") for n, h, t in arms])
    m.ellip((0, -1, c + 1), (23.5, 22.0, 21.0), "vein", "body", segs=(32, 22), noise=0.05, name="sac")
    for k in range(6):
        a = math.radians(k * 60 + 15)
        m.ellip((15 * math.cos(a), -3 + 12 * math.sin(a) * 0.6, c + 9 + 6 * math.sin(a)), (8.0, 7.0, 6.0), "flesh", "body",
                segs=(14, 10), noise=0.1, name="lobe")
    m.ellip((0, 15.5, c - 4), (13.5, 7.0, 8.0), "glow", "body", segs=(20, 12), name="maw")
    m.box((0, 19.5, c + 3.0), (22.0, 5.0, 3.0), "flesh", "body", bevel=0.45, taper=(0.8, 0.9), name="upperlip")
    for i in range(11):
        a = math.radians(-62 + i * 12.4)
        p = (12.5 * math.sin(a), 13 + 8.5 * math.cos(a), c + 1.5)
        m.horn(p, (0, 0.2, -1), 4.0 if i % 3 == 0 else 2.8, 0.75, "tooth", "body", segs=5, steps=3)
    m.ellip((0, 13.0, c - 12.0), (15.0, 10.0, 5.0), "flesh", "jaw", segs=(20, 12), name="jaw")
    for i in range(9):
        a = math.radians(-58 + i * 14.5)
        p = (12.5 * math.sin(a), 12.5 + 8.5 * math.cos(a), c - 9.5)
        m.horn(p, (0, 0.1, 1), 3.4, 0.7, "tooth", "jaw", segs=5, steps=3)
    m.ellip((0, 15.5, c + 12.5), (5.4, 3.6, 4.6), "flesh", "body", segs=(16, 10), name="socket")
    m.ellip((0, 17.6, c + 12.5), (3.8, 2.0, 3.4), "eye", "body", segs=(16, 10), name="eye")
    m.box((0, 18.6, c + 16.4), (9.0, 2.6, 2.4), "flesh", "body", bevel=0.45, rot=(-15, 0, 0), name="brow")
    for k in range(3):
        m.horn((0 + (k - 1) * 7, 4 - abs(k - 1) * 2, c + 21.5), ((k - 1) * 0.4, -0.2, 1), 6.0, 1.6, "tooth", "body",
               curl=(-10, 0, 0), segs=7, steps=4, name="horn")
    for n, h, t in arms:
        h, t = Vector(h), Vector(t)
        sx = 1 if h.x > 0 else -1
        m.tube([h, h.lerp(t, 0.5) + Vector((sx * 1.5, 0, 1)), t], [5.0, 3.8, 3.0], "flesh", n, segs=10, name="arm")
        d = (t - h).normalized()
        for k in range(3):
            dd = (d + Vector((0, 0, (k - 1) * 0.5))).normalized()
            m.horn(t + d * 1.0, dd, 3.4, 0.8, "claw", n, curl=(-15, 0, 0), segs=5, steps=3)
    finish(m, tex)
    flyer_clips(m, dict(arms=[n for n, _, _ in arms], centre=c, attack="spit", bob=2.0, lunge=3.0))
    return m
