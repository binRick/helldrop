"""Static props (no rig): the explosive barrel and the pickups.  Char space Doom units, origin on
the ground at the item's centre.  Pickups read at a glance: health = white + red, armour =
green, ammo = yellow/brass, weapons = gunmetal with a coloured stripe."""
import math

from mathutils import Vector

from hdlib import Model

BUILDERS = {}


def builder(fn):
    BUILDERS[fn.__name__] = fn
    return fn


def finish(m, tex, size=512):
    m.assemble()
    m.bake(tex, size=size)
    return m


def std_paints(m):
    m.paint("metal", (0.26, 0.27, 0.29), wear=0.9, wear_color=(0.70, 0.70, 0.73), seams=(3.0, 0.4))
    m.paint("dark", (0.10, 0.10, 0.11), wear=0.5, wear_color=(0.4, 0.4, 0.42))
    m.paint("brass", (0.86, 0.66, 0.24), wear=0.6, wear_color=(1.0, 0.92, 0.6), grime=0.4)
    m.paint("yellow", (0.95, 0.75, 0.10), wear=0.5, wear_color=(1.0, 0.95, 0.7), grime=0.5)
    m.paint("olive", (0.34, 0.38, 0.22), wear=0.7, wear_color=(0.66, 0.64, 0.55), seams=(3.5, 0.4))
    m.paint("red", (0.80, 0.08, 0.06), wear=0.4, wear_color=(1.0, 0.5, 0.4))
    m.paint("white", (0.92, 0.92, 0.90), wear=0.4, wear_color=(1, 1, 1), grime=0.5)
    m.paint("wood", (0.48, 0.28, 0.12), color2=(0.36, 0.2, 0.08), pattern="stripes", pscale=0.5, wear=0.5)


@builder
def barrel(name, tex):
    m = Model(name, 32.0, 10.0)
    std_paints(m)
    m.paint("drum", (0.30, 0.38, 0.30), color2=(0.22, 0.28, 0.22), pattern="mottle", pscale=4, wear=0.8,
            wear_color=(0.62, 0.62, 0.58), grime=0.8, grime_color=(0.15, 0.12, 0.05),
            spots=((0.40, 0.22, 0.08), 3.0, 0.8))
    m.paint("hazard", (0.95, 0.72, 0.06), color2=(0.06, 0.06, 0.05), pattern="stripes", pscale=0.9, wear=0.7,
            wear_color=(0.5, 0.5, 0.45), grime=0.6)
    m.paint("ooze", (0.45, 1.0, 0.18), glow=True)
    m.cyl((0, 0, 0.6), (0, 0, 29.6), 9.4, "drum", None, segs=28, name="drum")
    for z in (0.6, 10.0, 20.0, 29.4):
        m.cyl((0, 0, z - 0.7), (0, 0, z + 0.7), 9.95, "metal", None, segs=28, name="rim")
    m.cyl((0, 0, 13.5), (0, 0, 17.5), 9.6, "hazard", None, segs=28, name="band")
    m.cyl((0, 0, 29.6), (0, 0, 30.6), 8.4, "metal", None, segs=24, name="lid")
    m.ellip((0, 0, 30.6), (7.6, 7.6, 1.8), "ooze", None, segs=(20, 8), name="pool")
    for k in range(4):
        a = math.radians(30 + k * 85)
        x, y = 9.2 * math.cos(a), 9.2 * math.sin(a)
        n = Vector((math.cos(a), math.sin(a), 0))
        top = Vector((x, y, 30.2))
        m.tube([top + n * 0.5, top + n * 0.9 + Vector((0, 0, -3 - k)), top + n * 0.7 + Vector((0, 0, -7 - 2 * k))],
               [1.4, 1.0, 0.0], "ooze", None, segs=8, name="drip")
    m.cyl((4.0, 1.0, 30.4), (4.0, 1.0, 31.4), 1.6, "dark", None, segs=10, name="cap")
    return finish(m, tex)


@builder
def pk_medikit(name, tex):
    m = Model(name, 12.0, 12.0)
    std_paints(m)
    m.box((0, 0, 4.6), (20.0, 13.0, 9.0), "white", None, bevel=0.15, name="case")
    m.box((0, 0, 4.6), (20.4, 13.4, 1.2), "dark", None, bevel=0.3, name="seam")
    m.box((0, 0, 9.3), (10.0, 3.2, 0.6), "red", None, bevel=0.3)
    m.box((0, 0, 9.3), (3.2, 10.0, 0.6), "red", None, bevel=0.3)
    m.box((0, 6.7, 5.4), (6.0, 0.4, 1.8), "red", None, bevel=0.2)
    m.box((0, 6.7, 5.4), (1.8, 0.4, 6.0), "red", None, bevel=0.2)
    m.tube([Vector((-4.5, 0, 9.2)), Vector((-3.5, 0, 11.6)), Vector((3.5, 0, 11.6)), Vector((4.5, 0, 9.2))],
           [0.8] * 4, "dark", None, segs=8, name="handle")
    for x in (-7.0, 7.0):
        m.box((x, 6.6, 6.8), (2.0, 0.8, 2.2), "metal", None, bevel=0.3)
    return finish(m, tex)


@builder
def pk_stimpack(name, tex):
    m = Model(name, 6.0, 7.0)
    std_paints(m)
    m.cyl((-6.0, 0, 2.8), (5.0, 0, 2.8), 2.6, "white", None, segs=18, name="body")
    m.cyl((-1.6, 0, 2.8), (0.6, 0, 2.8), 2.75, "red", None, segs=18, name="band")
    m.cyl((5.0, 0, 2.8), (7.0, 0, 2.8), 1.8, "metal", None, segs=14, name="cap")
    m.cyl((7.0, 0, 2.8), (10.5, 0, 2.8), 0.25, "metal", None, segs=6, name="needle")
    m.cyl((-8.0, 0, 2.8), (-6.0, 0, 2.8), 1.4, "dark", None, segs=12, name="plunger")
    m.cyl((-8.6, 0, 2.8), (-8.0, 0, 2.8), 2.4, "dark", None, segs=12)
    m.box((3.0, 0, 5.3), (2.4, 0.8, 0.5), "red", None, bevel=0.2)
    m.box((3.0, 0, 5.3), (0.8, 2.4, 0.5), "red", None, bevel=0.2)
    return finish(m, tex)


@builder
def pk_armor(name, tex):
    m = Model(name, 20.0, 12.0)
    std_paints(m)
    m.paint("green", (0.16, 0.62, 0.24), color2=(0.10, 0.42, 0.16), pattern="mottle", pscale=4, wear=0.8,
            wear_color=(0.65, 0.85, 0.6), seams=(3.5, 0.5), grime=0.5)
    m.box((0, 0, 11.0), (16.0, 8.0, 14.0), "green", None, bevel=0.2, taper=(1.12, 1.0), name="chest")
    m.box((0, 4.4, 12.0), (8.0, 1.2, 8.0), "dark", None, bevel=0.3, name="sternum")
    m.box((0, 4.8, 12.6), (5.0, 0.6, 2.0), "yellow", None, bevel=0.3, name="badge")
    m.box((0, 0, 3.0), (14.0, 7.4, 4.0), "green", None, bevel=0.25, name="abs")
    for sx in (1, -1):
        m.box((sx * 8.6, 0, 17.0), (5.0, 9.0, 2.6), "green", None, rot=(0, sx * 25, 0), bevel=0.3, name="shoulder")
        m.box((sx * 4.4, 0, 18.6), (2.6, 8.4, 1.0), "dark", None, bevel=0.3)
    m.cyl((0, 0, 0.0), (0, 0, 1.2), 10.0, "dark", None, segs=24, name="stand")
    return finish(m, tex)


def gun_lying(m, kind):
    """A weapon lying on its side, barrel along +x."""
    z = 3.4
    if kind == "shotgun":
        m.box((-2, 0, z), (11, 2.6, 3.6), "metal", None, bevel=0.2, name="receiver")
        m.box((-11, 0, z - 0.6), (9, 2.4, 3.6), "wood", None, bevel=0.35, rot=(0, 6, 0), name="stock")
        m.cyl((3, 0.7, z + 1.0), (21, 0.7, z + 1.0), 0.9, "metal", None, segs=10)
        m.cyl((3, -0.7, z + 1.0), (21, -0.7, z + 1.0), 0.9, "metal", None, segs=10)
        m.box((10, 0, z - 0.7), (7, 3.2, 2.6), "wood", None, bevel=0.35, name="pump")
        m.box((-2, 1.35, z + 0.4), (6, 0.3, 0.9), "red", None, bevel=0.0)
    elif kind == "chaingun":
        m.box((-4, 0, z + 1.5), (14, 6, 6), "metal", None, bevel=0.2, name="housing")
        m.box((-4, 3.4, z + 1.5), (8, 1.8, 4.4), "brass", None, bevel=0.25)
        m.box((-6, 0, z + 5.2), (7, 1.2, 1.8), "dark", None, bevel=0.3, name="handle")
        for k in range(6):
            a = 2 * math.pi * k / 6
            m.cyl((3, 1.7 * math.cos(a), z + 1.5 + 1.7 * math.sin(a)), (20, 1.7 * math.cos(a), z + 1.5 + 1.7 * math.sin(a)),
                  0.6, "dark", None, segs=8)
        for x in (3.5, 12.0, 19.0):
            m.cyl((x, 0, z + 1.5), (x + 1.2, 0, z + 1.5), 2.7, "metal", None, segs=14)
    elif kind == "rlaunch":
        m.cyl((-14, 0, z + 2.0), (16, 0, z + 2.0), 3.6, "olive", None, segs=18, name="tube")
        m.cyl((14, 0, z + 2.0), (17, 0, z + 2.0), 4.2, "metal", None, segs=18)
        m.cyl((-15, 0, z + 2.0), (-12, 0, z + 2.0), 4.2, "metal", None, segs=18)
        m.cyl((16.9, 0, z + 2.0), (17.1, 0, z + 2.0), 3.0, "dark", None, segs=18)
        m.box((-2, 0, z - 2.5), (3.0, 2.2, 5.0), "dark", None, rot=(0, -15, 0), bevel=0.3, name="grip")
        m.box((5, 0, z + 6.0), (6.0, 1.6, 2.2), "dark", None, bevel=0.3, name="sight")
        m.box((0, 0, z + 2.0), (6.0, 7.6, 2.0), "yellow", None, bevel=0.3, name="stripe")


@builder
def pk_shotgun(name, tex):
    m = Model(name, 8.0, 16.0)
    std_paints(m)
    gun_lying(m, "shotgun")
    return finish(m, tex)


@builder
def pk_chaingun(name, tex):
    m = Model(name, 10.0, 16.0)
    std_paints(m)
    gun_lying(m, "chaingun")
    return finish(m, tex)


@builder
def pk_rlaunch(name, tex):
    m = Model(name, 12.0, 18.0)
    std_paints(m)
    gun_lying(m, "rlaunch")
    return finish(m, tex)


def round_(m, p, d=(0, 0, 1), length=3.0, r=0.6, case="brass", tip="brass"):
    p, d = Vector(p), Vector(d).normalized()
    m.cyl(tuple(p), tuple(p + d * length * 0.7), r, case, None, segs=8)
    m.cyl(tuple(p + d * length * 0.7), tuple(p + d * length), r, tip, None, r1=r * 0.35, segs=8)


@builder
def pk_clip(name, tex):
    m = Model(name, 10.0, 6.0)
    std_paints(m)
    m.box((0, 0, 4.0), (3.2, 6.0, 8.0), "dark", None, bevel=0.2, rot=(0, 0, 0), name="mag")
    m.box((0, 0, 0.6), (3.8, 6.8, 1.2), "metal", None, bevel=0.3)
    for k in range(3):
        round_(m, (0, -1.6 + 1.6 * k, 7.6 + (k % 2) * 0.4), d=(0, 1, 0.15), length=4.2, r=0.75)
    m.box((1.65, 0, 4.0), (0.2, 3.6, 4.8), "yellow", None, bevel=0.0)
    return finish(m, tex)


@builder
def pk_clipbox(name, tex):
    m = Model(name, 12.0, 12.0)
    std_paints(m)
    m.box((0, 0, 5.0), (18.0, 10.0, 10.0), "olive", None, bevel=0.12, name="crate")
    m.box((0, 0, 10.2), (18.4, 10.4, 0.8), "metal", None, bevel=0.3, name="lid")
    m.box((0, 5.1, 5.0), (12.0, 0.4, 3.6), "yellow", None, bevel=0.2, name="label")
    m.box((0, -5.1, 5.0), (12.0, 0.4, 3.6), "yellow", None, bevel=0.2)
    for k in range(5):
        round_(m, (-6 + 3 * k, -2.0, 10.4), d=(0.1, 0.2, 1), length=3.4, r=0.8)
        round_(m, (-4.5 + 3 * k, 1.8, 10.4), d=(-0.1, -0.1, 1), length=3.4, r=0.8)
    m.tube([Vector((-4, 0, 10.6)), Vector((-3, 0, 12.2)), Vector((3, 0, 12.2)), Vector((4, 0, 10.6))], [0.6] * 4,
           "dark", None, segs=6)
    return finish(m, tex)


@builder
def pk_shells(name, tex):
    m = Model(name, 8.0, 8.0)
    std_paints(m)
    for k, (x, y, a) in enumerate(((-3, -1.5, 0), (0, 1.5, 25), (3, -1, -20), (1, -4, 70))):
        d = Vector((math.cos(math.radians(a)), math.sin(math.radians(a)), 0))
        p = Vector((x, y, 1.3)) - d * 3
        m.cyl(tuple(p), tuple(p + d * 1.6), 1.35, "brass", None, segs=12)
        m.cyl(tuple(p + d * 1.6), tuple(p + d * 6.5), 1.25, "red", None, segs=12)
    m.cyl((-1.0, 0, 2.6), (-1.0, 0, 4.2), 1.35, "brass", None, segs=12)
    m.cyl((-1.0, 0, 4.2), (-1.0, 0, 9.0), 1.25, "red", None, segs=12)
    return finish(m, tex)


@builder
def pk_shellbox(name, tex):
    m = Model(name, 12.0, 12.0)
    std_paints(m)
    m.box((0, 0, 4.0), (16.0, 10.0, 8.0), "red", None, bevel=0.12, name="box")
    m.box((0, 5.1, 4.0), (10.0, 0.4, 4.0), "yellow", None, bevel=0.2)
    m.box((0, -5.1, 4.0), (10.0, 0.4, 4.0), "yellow", None, bevel=0.2)
    for i in range(5):
        for j in range(3):
            x, y = -5.6 + i * 2.8, -2.8 + j * 2.8
            m.cyl((x, y, 7.0), (x, y, 9.4), 1.2, "red", None, segs=10)
            m.cyl((x, y, 9.4), (x, y, 10.6), 1.25, "brass", None, segs=10)
    return finish(m, tex)


def rocket(m, p, d, length=22.0, r=2.0):
    p, d = Vector(p), Vector(d).normalized()
    m.cyl(tuple(p), tuple(p + d * length * 0.75), r, "olive", None, segs=14)
    m.cyl(tuple(p + d * length * 0.75), tuple(p + d * length), r, "red", None, r1=0.4, segs=14)
    m.cyl(tuple(p + d * length * 0.3), tuple(p + d * length * 0.36), r * 1.05, "yellow", None, segs=14)
    m.cyl(tuple(p - d * 1.2), tuple(p), r * 0.7, "dark", None, segs=10)
    side = d.cross(Vector((0, 0, 1)))
    if side.length < 1e-3:
        side = Vector((1, 0, 0))
    side.normalize()
    up = side.cross(d).normalized()
    for v in (side, -side, up, -up):
        m.box(tuple(p + d * 1.2 + v * (r + 0.8)), (0.3, 0.3, 0.3), "dark", None, bevel=0.0)
        m.tube([p + v * r, p + v * (r + 1.6) + d * 0.6, p + d * 3.0 + v * r], [0.25] * 3, "dark", None, segs=4)


@builder
def pk_rocket(name, tex):
    m = Model(name, 6.0, 12.0)
    std_paints(m)
    rocket(m, (-11, 0, 2.2), (1, 0, 0))
    return finish(m, tex)


@builder
def pk_rocketbox(name, tex):
    m = Model(name, 14.0, 14.0)
    std_paints(m)
    m.box((0, 0, 4.5), (26.0, 14.0, 9.0), "olive", None, bevel=0.12, name="crate")
    m.box((0, 7.1, 4.5), (16.0, 0.4, 3.6), "yellow", None, bevel=0.2)
    m.box((0, -7.1, 4.5), (16.0, 0.4, 3.6), "yellow", None, bevel=0.2)
    for k in range(3):
        rocket(m, (-11, -4.5 + 4.5 * k, 11.2), (1, 0, 0.02 * (k - 1)), length=22, r=2.0)
    return finish(m, tex)
