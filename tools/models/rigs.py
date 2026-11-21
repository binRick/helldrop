"""rigs -- biped / flyer skeletons and their procedural clips (idle, walk, attack, pain, death).

All poses are dicts {bone: Quaternion in char space (delta from rest, FK)} plus "_loc" (root
translation, char space Doom units).  Clips are sampled every 2 frames on a 60 fps timeline.
"""
import math

from mathutils import Quaternion, Vector

from hdlib import Q, seg, smooth

TAU = 2 * math.pi


def bump(t, a, b):
    """0 -> 1 -> 0 across [a, b] (sine bump)."""
    if t <= a or t >= b:
        return 0.0
    return math.sin(math.pi * (t - a) / (b - a))


def kick(t, at, rise=0.04, decay=0.22):
    """Sharp hit at 'at' with an exponential-ish decay (recoil / impact)."""
    if t < at - rise:
        return 0.0
    if t < at:
        return smooth((t - (at - rise)) / rise)
    return max(0.0, 1.0 - (t - at) / decay) ** 2


def V3(*a):
    return Vector(a)


def mix_pose(*poses):
    out = {}
    for p in poses:
        for k, v in p.items():
            if k == "_loc":
                out["_loc"] = tuple(a + b for a, b in zip(out.get("_loc", (0, 0, 0)), v))
            else:
                out[k] = out[k] @ v if k in out else v
    return out


# ============================================================ biped skeleton
def biped_joints(H=56.0, hip=0.5, hipw=0.085, waist=0.62, chest=0.75, neck=0.855, headb=0.875, headtop=1.0,
                 shoulder=0.815, shw=0.19, hunch=0.0, knee=0.27, kneefwd=0.015, ankle=0.06, foot=0.13,
                 stance=0.02, digi=False):
    s = H
    J = {}
    hz = hip * s

    def hy(z):
        return hunch * s * max(0.0, z - hz) / max(1e-3, s - hz)
    J["root"] = V3(0, 0, 0)
    J["pelvis"] = V3(0, 0, hz)
    for k, f in (("waist", waist), ("chest", chest), ("neck", neck), ("headb", headb), ("headtop", headtop)):
        J[k] = V3(0, hy(f * s), f * s)
    for side, sx in (("R", 1), ("L", -1)):
        J["sh." + side] = V3(sx * shw * s, hy(shoulder * s), shoulder * s)
        J["hip." + side] = V3(sx * hipw * s, 0, hz)
        fx = sx * (hipw + stance) * s
        if digi:
            J["knee." + side] = V3(fx, 0.10 * s, 0.36 * s)
            J["hock." + side] = V3(fx, -0.07 * s, 0.15 * s)
            J["ankle." + side] = V3(fx, -0.02 * s, 0.035 * s)
            J["toe." + side] = V3(fx, 0.08 * s, 0.0)
        else:
            J["knee." + side] = V3(fx * 0.98, kneefwd * s, knee * s)
            J["ankle." + side] = V3(fx, -0.005 * s, ankle * s)
            J["toe." + side] = V3(fx, foot * s, 0.012 * s)
    return J


def arm_chain(J, side, el, wr, tip):
    J["el." + side], J["wr." + side], J["tip." + side] = V3(*el), V3(*wr), V3(*tip)


def hang_arms(J, H, ua=0.19, fa=0.17, hand=0.075, out=10, fwd=6, elbow=20):
    s = H
    for side, sx in (("R", 1), ("L", -1)):
        sh = J["sh." + side]
        a_o, a_f = math.radians(out), math.radians(fwd)
        d1 = V3(sx * math.sin(a_o), math.sin(a_f), -math.cos(a_f) * math.cos(a_o)).normalized()
        a2 = math.radians(fwd + elbow)
        d2 = V3(sx * math.sin(a_o * 0.6), math.sin(a2), -math.cos(a2)).normalized()
        el = sh + d1 * ua * s
        wr = el + d2 * fa * s
        J["el." + side], J["wr." + side], J["tip." + side] = el, wr, wr + d2 * hand * s


def add_biped_bones(m, J, digi=False, jaw=None, extra=()):
    up = V3(0, 0, 4)
    m.bone("root", J["root"], J["root"] + up)
    m.bone("hips", J["pelvis"], J["pelvis"] + up, "root")
    m.bone("spine", J["waist"], J["chest"], "hips")
    m.bone("chest", J["chest"], J["neck"], "spine")
    m.bone("neck", J["neck"], J["headb"], "chest")
    m.bone("head", J["headb"], J["headtop"], "neck")
    if jaw:
        m.bone("jaw", jaw[0], jaw[1], "head")
    for s in ("R", "L"):
        m.bone("upperarm." + s, J["sh." + s], J["el." + s], "chest")
        m.bone("forearm." + s, J["el." + s], J["wr." + s], "upperarm." + s)
        m.bone("hand." + s, J["wr." + s], J["tip." + s], "forearm." + s)
        m.bone("thigh." + s, J["hip." + s], J["knee." + s], "hips")
        if digi:
            m.bone("shin." + s, J["knee." + s], J["hock." + s], "thigh." + s)
            m.bone("meta." + s, J["hock." + s], J["ankle." + s], "shin." + s)
            m.bone("foot." + s, J["ankle." + s], J["toe." + s], "meta." + s)
        else:
            m.bone("shin." + s, J["knee." + s], J["ankle." + s], "thigh." + s)
            m.bone("foot." + s, J["ankle." + s], J["toe." + s], "shin." + s)
    for b in extra:
        m.bone(*b)


# ============================================================ biped clips
DEFAULT_STYLE = dict(stride=26.0, knee=48.0, armswing=22.0, elbow_swing=12.0, bob=0.6, twist=6.0, gun=False,
                     attack="gun", death="back", breath=2.0, lean_walk=4.0, idle_frames=96, walk_frames=60,
                     attack_frames=42, pain_frames=18, death_frames=60, leglen=28.0, digi=False, jaw=False,
                     heavy=1.0, head_look=10.0)


def _legs(P, st, ph, amp=1.0):
    A, K = st["stride"] * amp, st["knee"] * amp
    for side, off in (("R", 0.0), ("L", math.pi)):
        f = ph + off
        th = A * math.sin(f)
        kn = K * max(0.0, math.cos(f)) ** 1.4 + 4.0 * amp
        P["thigh." + side] = Q(pitch=-th)
        P["shin." + side] = Q(pitch=kn)
        if st["digi"]:
            P["meta." + side] = Q(pitch=-kn * 0.75)
            P["foot." + side] = Q(pitch=th - kn * 0.25 + 8 * max(0.0, math.cos(f)))
        else:
            P["foot." + side] = Q(pitch=th - kn + 6 * max(0.0, math.cos(f)))
    return A


def biped_idle(st):
    def f(t):
        ph = TAU * t
        b = math.sin(ph)
        P = {"chest": Q(pitch=-st["breath"] * b * 0.6), "spine": Q(pitch=st["breath"] * 0.3 * b),
             "neck": Q(yaw=st["head_look"] * math.sin(ph + 0.6) * 0.5),
             "head": Q(yaw=st["head_look"] * math.sin(ph + 0.6) * 0.5, pitch=2.0 * math.sin(2 * ph))}
        for s, sx in (("R", 1), ("L", -1)):
            P["upperarm." + s] = Q(pitch=1.5 * math.sin(ph + 1.0), roll=sx * -1.5 * (1 + b) * 0.5)
            P["forearm." + s] = Q(pitch=-2.0 * (1 + math.sin(ph + 1.4)) * 0.5)
        if st["jaw"]:
            P["jaw"] = Q(pitch=3.0 * (1 + math.sin(2 * ph)))
        P["hips"] = Q(roll=1.2 * math.sin(ph))
        P["thigh.R"] = Q(roll=-1.2 * math.sin(ph))
        P["thigh.L"] = Q(roll=-1.2 * math.sin(ph))
        P["_loc"] = (0.35 * math.sin(ph), 0, 0)
        return P
    return f


def biped_walk(st):
    def f(t):
        ph = TAU * t
        P = {}
        A = _legs(P, st, ph)
        L = st["leglen"]
        drop = L * (1 - math.cos(math.radians(A * abs(math.sin(ph))) * 0.9))
        P["_loc"] = (0, 0, -drop - st["bob"] * 0.5 * (1 - math.cos(2 * ph)) * 0.3)
        P["hips"] = Q(yaw=st["twist"] * math.sin(ph), roll=2.0 * math.cos(ph))
        P["spine"] = Q(pitch=st["lean_walk"] * 0.5, yaw=-st["twist"] * 0.6 * math.sin(ph))
        P["chest"] = Q(pitch=st["lean_walk"] * 0.5 + 1.5 * math.cos(2 * ph), yaw=-st["twist"] * 0.5 * math.sin(ph))
        P["neck"] = Q(yaw=st["twist"] * 0.5 * math.sin(ph))
        P["head"] = Q(pitch=-st["lean_walk"] * 0.6 - 1.5 * math.cos(2 * ph))
        if st["gun"]:
            for s in ("R", "L"):
                P["upperarm." + s] = Q(pitch=2.5 * math.cos(2 * ph))
                P["forearm." + s] = Q(pitch=-1.5 * math.cos(2 * ph))
        else:
            for s, off in (("R", math.pi), ("L", 0.0)):
                a = math.sin(ph + off)
                P["upperarm." + s] = Q(pitch=-st["armswing"] * a)
                P["forearm." + s] = Q(pitch=-st["elbow_swing"] * (0.5 + 0.5 * max(0, a)))
        if st["jaw"]:
            P["jaw"] = Q(pitch=4.0 * (1 + math.sin(2 * ph)))
        return P
    return f


def biped_attack(st):
    kind = st["attack"]

    def f(t):
        P = {}
        if kind == "gun":
            aim = seg(t, 0.0, 0.35) * (1 - seg(t, 0.82, 1.0))
            k = kick(t, 0.6, 0.03, 0.25)
            P["spine"] = Q(pitch=4 * aim - 7 * k, yaw=-6 * aim)
            P["chest"] = Q(pitch=-5 * k, yaw=-4 * aim)
            P["head"] = Q(pitch=-3 * aim + 3 * k)
            for s in ("R", "L"):
                P["upperarm." + s] = Q(pitch=-14 * aim + 10 * k)
                P["forearm." + s] = Q(pitch=-8 * aim - 6 * k)
            P["thigh.R"] = Q(pitch=-6 * aim)
            P["thigh.L"] = Q(pitch=6 * aim)
            P["shin.L"] = Q(pitch=8 * aim)
            P["_loc"] = (0, -1.2 * k, -0.8 * aim)
        elif kind == "throw":
            wind = seg(t, 0.0, 0.42)
            hurl = seg(t, 0.45, 0.62)
            back = seg(t, 0.72, 1.0)
            w = wind * (1 - back)
            h = hurl * (1 - back)
            P["upperarm.R"] = Q(pitch=-150 * w + 105 * h, roll=-25 * w + 20 * h)
            P["forearm.R"] = Q(pitch=-70 * w + 60 * h)
            P["hand.R"] = Q(pitch=30 * w - 50 * h)
            P["upperarm.L"] = Q(pitch=-35 * w + 50 * h, roll=20 * w)
            P["forearm.L"] = Q(pitch=-30 * w)
            P["spine"] = Q(pitch=-8 * w + 22 * h, yaw=-22 * w + 34 * h)
            P["chest"] = Q(pitch=-6 * w + 10 * h, yaw=-10 * w + 14 * h)
            P["head"] = Q(pitch=6 * w - 12 * h, yaw=24 * w - 34 * h)
            P["thigh.R"] = Q(pitch=10 * w - 14 * h)
            P["thigh.L"] = Q(pitch=-14 * w + 24 * h)
            P["shin.L"] = Q(pitch=10 * w)
            P["shin.R"] = Q(pitch=12 * h)
            P["_loc"] = (0, -1 * w + 3 * h, -1.5 * h)
        elif kind == "bite":
            rear = seg(t, 0.0, 0.42) * (1 - seg(t, 0.5, 0.6))
            lunge = seg(t, 0.45, 0.6) * (1 - seg(t, 0.7, 1.0))
            P["spine"] = Q(pitch=-10 * rear + 18 * lunge)
            P["chest"] = Q(pitch=-8 * rear + 12 * lunge)
            P["neck"] = Q(pitch=-12 * rear + 10 * lunge)
            P["head"] = Q(pitch=-14 * rear + 14 * lunge)
            jaw = 42 * seg(t, 0.05, 0.4) * (1 - seg(t, 0.56, 0.62)) + 10 * bump(t, 0.62, 0.9)
            P["jaw"] = Q(pitch=jaw)
            for s in ("R", "L"):
                P["upperarm." + s] = Q(pitch=-30 * lunge + 15 * rear)
                P["forearm." + s] = Q(pitch=-30 * lunge)
            P["thigh.R"] = Q(pitch=-28 * lunge + 8 * rear)
            P["shin.R"] = Q(pitch=20 * lunge)
            P["thigh.L"] = Q(pitch=18 * lunge)
            P["shin.L"] = Q(pitch=10 * lunge + 10 * rear)
            P["_loc"] = (0, 7 * lunge - 2 * rear, -2.5 * lunge)
        elif kind == "cast":
            raise_ = seg(t, 0.0, 0.45) * (1 - seg(t, 0.75, 1.0))
            thrust = seg(t, 0.5, 0.62) * (1 - seg(t, 0.75, 1.0))
            for s, sx in (("R", 1), ("L", -1)):
                P["upperarm." + s] = Q(pitch=-150 * raise_ + 70 * thrust, roll=sx * -40 * raise_ + sx * 30 * thrust)
                P["forearm." + s] = Q(pitch=-25 * raise_ + 25 * thrust)
                P["hand." + s] = Q(pitch=30 * thrust - 20 * raise_)
            P["spine"] = Q(pitch=-10 * raise_ + 20 * thrust)
            P["chest"] = Q(pitch=-8 * raise_ + 10 * thrust)
            P["head"] = Q(pitch=-18 * raise_ + 22 * thrust)
            P["_loc"] = (0, 2 * thrust, 1.5 * raise_ - 1.5 * thrust)
        elif kind == "cannons":
            aim = seg(t, 0.0, 0.4) * (1 - seg(t, 0.8, 1.0))
            k = kick(t, 0.6, 0.03, 0.3)
            for s in ("R", "L"):
                P["upperarm." + s] = Q(pitch=-35 * aim + 14 * k)
                P["forearm." + s] = Q(pitch=-30 * aim - 10 * k)
            P["spine"] = Q(pitch=-5 * aim - 6 * k)
            P["chest"] = Q(pitch=-4 * k)
            P["head"] = Q(pitch=5 * aim)
            P["_loc"] = (0, -1.5 * k, -1.0 * aim)
        elif kind == "punch":
            wind = seg(t, 0.0, 0.42) * (1 - seg(t, 0.5, 0.6))
            hit = seg(t, 0.48, 0.6) * (1 - seg(t, 0.75, 1.0))
            P["upperarm.R"] = Q(pitch=25 * wind - 85 * hit, roll=-20 * wind)
            P["forearm.R"] = Q(pitch=-90 * wind + 10 * hit)
            P["upperarm.L"] = Q(pitch=-30 * wind + 20 * hit)
            P["forearm.L"] = Q(pitch=-50 * wind)
            P["spine"] = Q(yaw=-25 * wind + 30 * hit, pitch=10 * hit)
            P["chest"] = Q(yaw=-10 * wind + 12 * hit)
            P["head"] = Q(yaw=25 * wind - 30 * hit)
            P["thigh.L"] = Q(pitch=-20 * hit)
            P["shin.L"] = Q(pitch=10 * hit)
            P["thigh.R"] = Q(pitch=12 * hit)
            P["_loc"] = (0, 4 * hit, -1.2 * hit)
        if st["jaw"] and kind != "bite":
            P["jaw"] = Q(pitch=25 * bump(t, 0.3, 0.85))
        return P
    return f


def biped_pain(st):
    def f(t):
        k = kick(t, 0.22, 0.2, 0.75)
        P = {"spine": Q(pitch=-9 * k, roll=4 * k), "chest": Q(pitch=-8 * k), "neck": Q(pitch=-6 * k),
             "head": Q(pitch=-10 * k, yaw=12 * k)}
        for s, sx in (("R", 1), ("L", -1)):
            P["upperarm." + s] = Q(pitch=-12 * k, roll=-sx * 16 * k)
            P["forearm." + s] = Q(pitch=-14 * k)
            P["thigh." + s] = Q(pitch=-6 * k)
            P["shin." + s] = Q(pitch=10 * k)
        if st["jaw"]:
            P["jaw"] = Q(pitch=30 * k)
        P["_loc"] = (0, -2.0 * k, -1.0 * k)
        return P
    return f


def biped_death(st):
    kind = st["death"]

    afwd = st.get("arm_fwd", 30.0)

    def f(t):
        P = {}
        rec = seg(t, 0.0, 0.18) * (1 - seg(t, 0.35, 0.7))
        fall = seg(t, 0.18, 0.72) ** 1.6
        land = seg(t, 0.68, 0.78)
        bounce = bump(t, 0.72, 0.9)
        relax = seg(t, 0.6, 1.0)
        if kind == "back":
            P["root"] = Q(pitch=-92 * fall + 5 * bounce)
            P["spine"] = Q(pitch=-14 * rec + 8 * bump(t, 0.2, 0.7))
            P["chest"] = Q(pitch=-12 * rec)
            P["neck"] = Q(pitch=-10 * rec + 12 * relax, yaw=10 * relax)
            P["head"] = Q(pitch=-14 * rec + 6 * relax, yaw=26 * relax)
            for s, sx in (("R", 1), ("L", -1)):
                P["upperarm." + s] = Q(pitch=-60 * rec - 30 * bump(t, 0.3, 0.8) - 10 * relax,
                                       roll=-sx * (25 * rec + 62 * relax))
                P["forearm." + s] = Q(pitch=-25 * rec - 18 * relax + afwd * relax)
                P["hand." + s] = Q(pitch=10 * relax)
                kn = 38 * bump(t, 0.12, 0.72) + 8 * relax
                P["thigh." + s] = Q(pitch=-28 * bump(t, 0.12, 0.72) - 4 * relax, roll=-sx * 9 * relax)
                P["shin." + s] = Q(pitch=kn)
                if st["digi"]:
                    P["meta." + s] = Q(pitch=-kn * 0.5)
                P["foot." + s] = Q(pitch=25 * relax, roll=-sx * 15 * relax)
            if st["jaw"]:
                P["jaw"] = Q(pitch=30 * seg(t, 0.1, 0.5))
            P["_loc"] = (0, -3 * rec - 2 * fall, 0)
        elif kind == "front":
            P["root"] = Q(pitch=90 * fall - 4 * bounce)
            P["spine"] = Q(pitch=12 * rec - 4 * relax)
            P["chest"] = Q(pitch=10 * rec - 4 * relax)
            P["neck"] = Q(pitch=12 * rec - 20 * relax, yaw=20 * relax)
            P["head"] = Q(pitch=-10 * relax, yaw=55 * relax)
            for s, sx in (("R", 1), ("L", -1)):
                P["upperarm." + s] = Q(pitch=-50 * bump(t, 0.25, 0.8) + 10 * relax, roll=-sx * (20 + 50 * relax))
                P["forearm." + s] = Q(pitch=-30 * relax + afwd * relax)
                P["thigh." + s] = Q(pitch=-20 * bump(t, 0.1, 0.6), roll=-sx * 8 * relax)
                P["shin." + s] = Q(pitch=30 * bump(t, 0.1, 0.7) + 12 * relax)
                if st["digi"]:
                    P["meta." + s] = Q(pitch=-10 * relax)
                P["foot." + s] = Q(pitch=65 * relax)
            if st["jaw"]:
                P["jaw"] = Q(pitch=25 * seg(t, 0.1, 0.5))
            P["_loc"] = (0, 3 * rec + 2 * fall, 0)
        elif kind == "side":
            P["root"] = Q(roll=88 * fall - 4 * bounce, pitch=10 * fall)
            P["spine"] = Q(pitch=-10 * rec + 12 * relax, roll=-8 * relax)
            P["chest"] = Q(pitch=8 * relax)
            P["neck"] = Q(roll=-10 * relax, pitch=10 * relax)
            P["head"] = Q(roll=-14 * relax, pitch=-10 * rec)
            for s, sx in (("R", 1), ("L", -1)):
                P["upperarm." + s] = Q(pitch=-30 * rec - 20 * relax, roll=-sx * 20 * rec)
                P["forearm." + s] = Q(pitch=-30 * relax + afwd * 0.6 * relax)
                P["thigh." + s] = Q(pitch=(-28 if s == "R" else 22) * relax - 15 * bump(t, 0.1, 0.6), roll=-sx * 6 * relax)
                P["shin." + s] = Q(pitch=12 * relax)
                if st["digi"]:
                    P["meta." + s] = Q(pitch=-20 * relax)
            if st["jaw"]:
                P["jaw"] = Q(pitch=35 * seg(t, 0.1, 0.5))
            P["_loc"] = (-2 * fall, 0, 0)
        return P
    return f


def biped_clips(m, st_over):
    st = dict(DEFAULT_STYLE)
    st.update(st_over)
    acts = [m.clip("idle", st["idle_frames"], biped_idle(st)),
            m.clip("walk", st["walk_frames"], biped_walk(st)),
            m.clip("attack", st["attack_frames"], biped_attack(st)),
            m.clip("pain", st["pain_frames"], biped_pain(st), step=1),
            m.clip("death", st["death_frames"], biped_death(st))]
    for a in acts:
        m.ground_fix(a, exact_from=(0.75 if a.name == "death" else 1.01))
    m.finish_clips(acts)
    return acts


# ============================================================ flyer skeleton + clips
def add_flyer_bones(m, c, r, jaw=None, extra=()):
    """root at the ground under the creature; 'body' at the hover centre c (Doom units)."""
    m.bone("root", (0, 0, 0), (0, 0, 4))
    m.bone("body", (0, 0, c), (0, 0, c + r * 0.6), "root")
    if jaw:
        m.bone("jaw", jaw[0], jaw[1], "body")
    for b in extra:
        m.bone(*b)


FLY_STYLE = dict(bob=1.6, attack="spit", centre=28.0, arms=(), idle_frames=120, walk_frames=72,
                 attack_frames=42, pain_frames=18, death_frames=60, lunge=6.0)


def flyer_clips(m, st_over):
    st = dict(FLY_STYLE)
    st.update(st_over)
    arms = st["arms"]

    def arm_pose(P, amp, ph):
        for i, b in enumerate(arms):
            P[b] = Q(pitch=amp * math.sin(ph + i * 1.3), roll=amp * 0.5 * math.cos(ph + i))

    def idle(t):
        ph = TAU * t
        P = {"body": Q(yaw=4 * math.sin(ph), pitch=2 * math.sin(2 * ph)),
             "jaw": Q(pitch=3 * (1 + math.sin(2 * ph)))}
        arm_pose(P, 8, 2 * ph)
        P["_loc"] = (0, 0, st["bob"] * math.sin(2 * ph))
        return P

    def walk(t):
        ph = TAU * t
        P = {"body": Q(pitch=9 + 2 * math.sin(2 * ph), roll=4 * math.sin(ph)), "jaw": Q(pitch=4 * (1 + math.sin(2 * ph)))}
        arm_pose(P, 14, 2 * ph)
        P["_loc"] = (0.6 * math.sin(ph), 0, st["bob"] * 1.4 * math.sin(2 * ph))
        return P

    def attack(t):
        rear = seg(t, 0.0, 0.45) * (1 - seg(t, 0.52, 0.62))
        hit = seg(t, 0.5, 0.62) * (1 - seg(t, 0.72, 1.0))
        jaw = seg(t, 0.1, 0.55) * (1 - seg(t, 0.75, 1.0))
        if st["attack"] == "charge":
            jaw = seg(t, 0.2, 0.5) * (1 - seg(t, 0.62, 0.7)) + 0.3 * bump(t, 0.7, 1.0)
        P = {"body": Q(pitch=-14 * rear + 18 * hit), "jaw": Q(pitch=46 * jaw)}
        arm_pose(P, 25 * rear, 6 * t)
        P["_loc"] = (0, -2 * rear + st["lunge"] * hit, 1.5 * rear - 1.5 * hit)
        return P

    def pain(t):
        k = kick(t, 0.22, 0.2, 0.75)
        P = {"body": Q(pitch=-16 * k, roll=12 * k, yaw=8 * k), "jaw": Q(pitch=20 * k)}
        arm_pose(P, 30 * k, 3.0)
        P["_loc"] = (1.5 * k, -2.5 * k, 1.0 * k)
        return P

    c = st["centre"]

    def death(t):
        jolt = seg(t, 0.0, 0.15) * (1 - seg(t, 0.2, 0.4))
        fall = seg(t, 0.2, 0.7) ** 1.8
        bounce = bump(t, 0.7, 0.86)
        relax = seg(t, 0.55, 1.0)
        P = {"body": Q(pitch=-18 * jolt + 22 * fall - 6 * bounce, roll=30 * fall + 5 * bounce, yaw=20 * fall),
             "jaw": Q(pitch=15 * jolt + 34 * relax)}
        for i, b in enumerate(arms):
            P[b] = Q(pitch=40 * relax * (1 if i % 2 else -1), roll=30 * relax)
        P["_loc"] = (0, -1.5 * fall, 3 * jolt - c * fall + 2.0 * bounce)
        return P

    acts = [m.clip("idle", st["idle_frames"], idle), m.clip("walk", st["walk_frames"], walk),
            m.clip("attack", st["attack_frames"], attack), m.clip("pain", st["pain_frames"], pain, step=1),
            m.clip("death", st["death_frames"], death)]
    m.ground_fix(acts[-1], exact_from=0.7)
    m.finish_clips(acts)
    return acts
