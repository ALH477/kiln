"""
anim.py - what the first pass was missing.

glTF LINEAR interpolation between two poses is a straight line, which is why
2-key animation reads as robotic. Everything here resamples a small number of
authored poses into a denser eased key list, so the exported LINEAR curve
traces an eased arc. It costs keyframe data (cheap) and buys weight.

Three things that make the difference:
  ease        - anticipation and settle instead of constant velocity
  overlap     - chains (tail, wings, ears) lag behind their parent
  counter     - hips and shoulders rotate against each other on locomotion
"""
import math
from gltfkit import euler, quat_mul, quat_axis

TAU = math.pi * 2


# ------------------------------------------------------------------- easings
def linear(t):    return t
def smooth(t):    return t * t * (3 - 2 * t)
def smoother(t):  return t * t * t * (t * (t * 6 - 15) + 10)
def ease_in(t):   return t * t * t
def ease_out(t):  return 1 - (1 - t) ** 3
def snap(t):      return 1 - (1 - t) ** 5          # very fast out, long settle
def anticipate(t):
    """pulls slightly backwards before launching - the single biggest win"""
    s = 1.70158
    return t * t * ((s + 1) * t - s)
def overshoot(t):
    s = 1.35
    u = t - 1
    return u * u * ((s + 1) * u + s) + 1


def _slerp(a, b, t):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    d = ax*bx + ay*by + az*bz + aw*bw
    if d < 0:
        bx, by, bz, bw, d = -bx, -by, -bz, -bw, -d
    if d > 0.9995:
        r = [ax+(bx-ax)*t, ay+(by-ay)*t, az+(bz-az)*t, aw+(bw-aw)*t]
    else:
        th = math.acos(max(-1.0, min(1.0, d)))
        st = math.sin(th) or 1e-9
        c0 = math.sin((1-t)*th)/st; c1 = math.sin(t*th)/st
        r = [ax*c0+bx*c1, ay*c0+by*c1, az*c0+bz*c1, aw*c0+bw*c1]
    n = math.sqrt(sum(v*v for v in r)) or 1.0
    return [v/n for v in r]


# ------------------------------------------------- resample authored poses
def tkeys(poses, sub=5, ease=smooth):
    """poses: [(time, (x,y,z))] -> denser eased translation keys."""
    out = []
    for i in range(len(poses) - 1):
        t0, v0 = poses[i]; t1, v1 = poses[i + 1]
        e = ease[i] if isinstance(ease, (list, tuple)) else ease
        for s in range(sub):
            a = s / sub
            k = e(a)
            out.append((t0 + (t1 - t0) * a,
                        [v0[j] + (v1[j] - v0[j]) * k for j in range(3)]))
    out.append((poses[-1][0], list(poses[-1][1])))
    return out


def rkeys(poses, sub=5, ease=smooth):
    """poses: [(time, (rx,ry,rz) euler radians)] -> denser eased quat keys."""
    q = [(t, euler(*e)) for t, e in poses]
    out = []
    for i in range(len(q) - 1):
        t0, q0 = q[i]; t1, q1 = q[i + 1]
        e = ease[i] if isinstance(ease, (list, tuple)) else ease
        for s in range(sub):
            a = s / sub
            out.append((t0 + (t1 - t0) * a, _slerp(q0, q1, e(a))))
    out.append((q[-1][0], list(q[-1][1])))
    return out


def skeys(poses, sub=5, ease=smooth):
    return tkeys(poses, sub, ease)


# ---------------------------------------------------------- periodic motion
def osc_rot(period, amp, axis=0, phase=0.0, base=(0, 0, 0), steps=16, harm2=0.0):
    """Sine (plus optional 2nd harmonic) rotation cycle. Loops seamlessly."""
    out = []
    for i in range(steps + 1):
        a = TAU * i / steps
        d = math.sin(a + phase) * amp + math.sin(2 * a + phase) * amp * harm2
        e = list(base); e[axis] += d
        out.append((period * i / steps, euler(*e)))
    return out


def osc_pos(period, amp, axis=1, phase=0.0, base=(0, 0, 0), steps=16, harm2=0.0):
    out = []
    for i in range(steps + 1):
        a = TAU * i / steps
        d = math.sin(a + phase) * amp + math.sin(2 * a + phase) * amp * harm2
        v = list(base); v[axis] += d
        out.append((period * i / steps, v))
    return out


def chain(joints, period, amp, axis=0, lag=0.55, decay=0.82, base=None,
          steps=16, phase=0.0):
    """
    Follow-through down a chain. Each link is phase-delayed from its parent and
    swings a little less. This is the whole reason a tail reads as a tail
    instead of a rod - the parent leads, the tip arrives late.
    Returns {joint: {'rotation': keys}}.
    """
    tracks, a, ph = {}, amp, phase
    for i, j in enumerate(joints):
        b = base[i] if base else (0, 0, 0)
        tracks[j] = {"rotation": osc_rot(period, a, axis, ph, b, steps)}
        a *= decay
        ph -= lag
    return tracks


def chain_follow(joints, poses, lag_frac=0.10, sub=4, ease=smooth, decay=0.80):
    """
    Follow-through on a one-shot action: hand the tip the same pose list, but
    shifted later in time and scaled down. Cheap, and reads correctly.
    poses: [(time,(rx,ry,rz))]
    """
    tracks, sc, shift = {}, 1.0, 0.0
    for j in joints:
        p = [(t + shift, tuple(v * sc for v in e)) for t, e in poses]
        tracks[j] = {"rotation": rkeys(p, sub, ease)}
        sc *= decay
        shift += lag_frac * (poses[-1][0] - poses[0][0])
    return tracks


# --------------------------------------------------------------- locomotion
def biped_walk(period, hip, thigh_l, shin_l, thigh_r, shin_r,
               torso=None, arm_l=None, arm_r=None, steps=16,
               stride=0.85, lift=0.55, bounce=0.055, hip_y=1.0, sway=0.10,
               digitigrade=0.45):
    """
    A real walk cycle rather than two poses. Contact / down / passing / up,
    with the hip bouncing at twice leg frequency and the torso counter-rotating
    against the hips. `digitigrade` bends the shin permanently, which is what
    makes a demon leg read as goat-legged instead of human.
    """
    tracks = {}
    th_l, sh_l, th_r, sh_r, hp, hpos = [], [], [], [], [], []
    to, al, ar = [], [], []
    for i in range(steps + 1):
        a = TAU * i / steps
        t = period * i / steps
        # thigh swings forward/back; shin folds on the up-swing only
        for (lst_t, lst_s, ph) in ((th_l, sh_l, 0.0), (th_r, sh_r, math.pi)):
            sw = math.sin(a + ph)
            fold = max(0.0, -math.cos(a + ph))
            lst_t.append((t, (sw * stride, 0, 0)))
            lst_s.append((t, (-digitigrade - fold * lift, 0, 0)))
        hpos.append((t, (math.sin(a) * sway * 0.5,
                         hip_y + abs(math.sin(a)) * bounce - bounce * 0.5, 0)))
        hp.append((t, (0, math.sin(a) * 0.10, math.sin(a) * sway)))
        to.append((t, (0.06, -math.sin(a) * 0.13, -math.sin(a) * sway * 0.6)))
        al.append((t, (-math.sin(a) * 0.55, 0, 0.30)))
        ar.append((t, (math.sin(a) * 0.55, 0, -0.30)))
    q = lambda L: [(t, euler(*e)) for t, e in L]
    tracks[thigh_l] = {"rotation": q(th_l)}
    tracks[shin_l]  = {"rotation": q(sh_l)}
    tracks[thigh_r] = {"rotation": q(th_r)}
    tracks[shin_r]  = {"rotation": q(sh_r)}
    tracks[hip]     = {"rotation": q(hp), "translation": [(t, list(v)) for t, v in hpos]}
    if torso is not None: tracks[torso] = {"rotation": q(to)}
    if arm_l is not None: tracks[arm_l] = {"rotation": q(al)}
    if arm_r is not None: tracks[arm_r] = {"rotation": q(ar)}
    return tracks


def quad_run(period, hip, legs, spine=None, steps=16, reach=0.95, fold=0.75,
             bound=0.16, base_y=0.86):
    """
    Bounding gait for a quadruped. legs is
    [(shoulder_l, fore_l), (shoulder_r, fore_r), (hip_l, hind_l), (hip_r, hind_r)]
    Fores and hinds are half a cycle apart, and the spine flexes at 2x so the
    animal gathers and extends.
    """
    tracks = {}
    phases = [0.0, math.pi * 0.12, math.pi, math.pi * 1.12]
    for (up, lo), ph in zip(legs, phases):
        U, L = [], []
        for i in range(steps + 1):
            a = TAU * i / steps; t = period * i / steps
            U.append((t, euler(math.sin(a + ph) * reach, 0, 0)))
            L.append((t, euler(-fold - max(0, -math.cos(a + ph)) * fold, 0, 0)))
        tracks[up] = {"rotation": U}
        tracks[lo] = {"rotation": L}
    H, S = [], []
    for i in range(steps + 1):
        a = TAU * i / steps; t = period * i / steps
        H.append((t, [0, base_y + abs(math.sin(a)) * bound, 0]))
        S.append((t, euler(math.sin(2 * a) * 0.18, 0, 0)))
    tracks[hip] = {"translation": H}
    if spine is not None: tracks[spine] = {"rotation": S}
    return tracks


def breathe(joint, period, amt=0.05, steps=12, phase=0.0):
    out = []
    for i in range(steps + 1):
        a = TAU * i / steps
        s = 1.0 + math.sin(a + phase) * amt
        out.append((period * i / steps, [s, 1.0 - amt * 0.4 * math.sin(a + phase), s]))
    return out


def flap(period, shoulder, mid, tip, amp=0.95, steps=16, lag=0.9, base=0.0):
    """Wing beat with the membrane arriving late - the classic bat read."""
    tr = {}
    for k, (j, sc, ph) in enumerate(((shoulder, 1.0, 0.0), (mid, 0.72, -lag),
                                     (tip, 0.50, -lag * 2))):
        K = []
        for i in range(steps + 1):
            a = TAU * i / steps; t = period * i / steps
            d = math.sin(a + ph)
            # downstroke is faster than the recovery
            d = d if d < 0 else d ** 0.65
            K.append((t, euler(0, 0, base + d * amp * sc)))
        tr[j] = {"rotation": K}
    return tr
