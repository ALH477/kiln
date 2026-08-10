# SPDX-License-Identifier: MPL-2.0
"""rider.py — the shared rider station.

One set of riding animations is authored on the goblin (goblin.py's Ride*
actions) and plays on BOTH vehicles (vehicles.py's go-kart and motorcycle).
That only works if the two vehicles put their controls in the same place
relative to the rider, so this module is where that agreement is written
down, and both sides import it rather than each carrying its own copy of
the numbers.

── The offsets ────────────────────────────────────────────────────────────
Everything is measured from the rider's HIP — the world position his root
bone's head lands on when he is mounted. Each vehicle declares its own hip
point (a kart's is low and far back, a bike's is high and mid-frame); the
grips and the footrests are then placed at the same offsets from it.

    GRIP  (±0.28, +0.60, +0.36)   where each hand closes
    REST  (±0.20, +0.42, -0.24)   where each ANKLE sits
    TREAD (0, +0.10, -0.13)       peg/pedal surface, relative to the ankle

── Why a kart and a motorcycle can share a pose at all ────────────────────
They mostly cannot. A sportbike rider is folded forward over a tank with his
knees tucked back and up; a kart driver is nearly supine with his legs
straight out in front. No single pose is both, and pretending otherwise
gives you a rider who floats above one vehicle and clips through the other.

The exception is a CHOPPER: ape-hanger bars bring the grips back and up to
about where a kart's wheel sits relative to the hips, and forward controls
put the feet out front instead of underneath. That is the entire reason the
bike in vehicles.py is a chopper rather than the sportbike it started as —
the silhouette was chosen to make the shared animation honest, not the other
way round. It also happens to be the right bike for a game about stoner
goblins, which is what made the trade easy.

── The tolerance ──────────────────────────────────────────────────────────
These offsets are what the vehicles are BUILT to, not a runtime constraint,
and nothing enforces them at load time — the check is
tools/blender/test_rider.py, which assembles the goblin in his Ride pose
against each vehicle on the host and measures how far each hand and foot
ends up from the control it is supposed to be holding. MAX_SLOP is the
distance that test allows: about a third of a hand's width, which is close
enough that the contact reads as contact at the board camera's distance and
loose enough that neither vehicle has to be built to a millimetre.
"""

# Offsets from the rider's hip, in Blender units. (half-spacing across,
# forward, up).
#
# GRIP is where the hand bone's tip lands. REST is where the ANKLE lands —
# not the sole, and not the toe. The first version measured the foot bone's
# TAIL, which on this rig is the toe, and then asked the toe to sit exactly
# on the pedal; the leg dutifully over-rotated to put it there and the heel
# ended up in the air. The ankle is the joint the pose actually controls, so
# it is the one the contract names, and TREAD is the small fixed drop from
# there down to the surface the boot rests on.
# REST is close in because these are SHORT legs: hip to ankle is 0.52 on
# this rig, and the first pass put the footrests 0.69 away — unreachable, and
# the solver dutifully reported the least-bad compromise rather than failing,
# which is what an over-extended leg looks like from the inside. A station is
# only a contract if the body it is written for can actually meet it.
GRIP = (0.28, 0.60, 0.36)
REST = (0.20, 0.42, -0.24)
TREAD = (0.0, 0.10, -0.13)

# Where each vehicle seats its rider. Declared here rather than in
# vehicles.py so that a change to one cannot silently stop matching the
# other — both vehicles and the goblin read these same three lines.
KART_HIP = (0.0, -0.76, 0.62)
BIKE_HIP = (0.0, -0.58, 1.34)

# How far a hand or foot may land from its control before test_rider.py
# fails. A goblin hand is ~0.24 across, so this is roughly a third of one.
MAX_SLOP = 0.09


def _pair(hip, offset):
    """The left and right world points for an offset from `hip`."""
    dx, dy, dz = offset
    return [(hip[0] + dx, hip[1] + dy, hip[2] + dz),
            (hip[0] - dx, hip[1] + dy, hip[2] + dz)]


def grips(hip):
    """[(left), (right)] world grip points for a vehicle seated at `hip`."""
    return _pair(hip, GRIP)


def rests(hip):
    """[(left), (right)] world ANKLE points for a vehicle at `hip`."""
    return _pair(hip, REST)


def treads(hip):
    """[(left), (right)] world pedal/peg surface points — where the vehicle
    puts actual geometry, a short drop forward of the ankle."""
    return _pair(hip, (REST[0] + TREAD[0], REST[1] + TREAD[1],
                       REST[2] + TREAD[2]))
