# Scene geometry review — PetaByte Madness

> **For Hermes:** This plan *is* the review. Findings first (evidence, severity, axis). Then fix tasks in risk order. Do not invent a fifth lab. Do not walk the island. Do not `nix build` the ROM on the laptop unless the user asks.

**Goal:** Make collision, draw meshes, and interactables describe the same rooms — starting with the two rooms the player actually walks.

**Architecture:** There are two walkable rooms and they must stay two rooms. PLAY is `assets/pm_lab.map` (Quake brushes → `kiln_clip` + `kiln_map` faces + demon spawns). The intro LAB is `dank_lab.obj` (drawn) plus a handful of hand AABBs in `src/pm_lab.c` (collided). `tools/blender/pm_world.py`'s `build_lab()` is a third room that nothing draws — leave it unused. The island is a hub mesh for flyover/attract, not a clip world.

**Tech stack:** Quake `.map` text, `kiln_map` / `kiln_clip` AABB brushes, Python generators (`tools/gen_lab_map.py`, `tools/dank_lab_gen.py`, `tools/blender/pm_world.py`), 64 world units = 1 metre.

**Repo:** `/home/asher/Documents/PetaByte-Madness` branch `restore-the-build`. Local only — no remotes. Do not push.

---

## Findings

Every claim has a `file:line`. Axes: **durability** (silent drift), **determinism** (gates), **usability** (player-facing), **YAGNI** (do not build).

### Sound — keep

- PLAY clip world is reinstalled when entering PLAY after LAB drops it (`src/main.c:344-359`). That is the right ownership split.
- Corridor length is load-bearing for the veil rebate: `Z0,Z1 = -100,900` plus 20-unit end caps = 1040 units; `pm_veil_init(..., 1000, 760)` in `src/main.c:200`; comment in `tools/gen_lab_map.py:31-35`. Keep that relationship.
- Gargoyle spawns sit on plinth Z values that exist (`tools/gen_lab_map.py:70-73` vs entities `:87-88`).
- Island / lab generators emit headers and `nix/checks/pm-gen-headers.nix` diffs them. `tools/blender/test_world.py` asserts flat walkable surfaces. Keep that discipline; extend it to the corridor map.
- `pm_world_gen.h:45-59` already renamed unused lab extents to `PM_WORLD_LAB_*` so they cannot clobber `PM_LAB_*`. Do not reintroduce `PM_LAB_X0` from `pm_world.py`.

### F1 — HIGH / usability — MRI use-volume is in a different room from the MRI

`src/pm_lab.c:146` spawns the MRI actor at `{-330, 40, 60}`.
`src/pm_lab_gen.h:49-51` places the drawn scanner at `PM_MRI_X = -44.8`, mouth at `PM_MRI_Z_FACE = -35.2`.

Delta X ≈ 285 units ≈ **4.5 m**. `kiln_context_scan` will offer USE on an empty patch of floor. The button the design insists the player press (`src/pm_lab.h:7-8`) is not on the machine.

Same file admits the class of bug: `src/pm_lab.c:138-140` `TUNE:` notes and notes at `:141-144` are bbox guesses, not on the desk (`PM_DESK_*` in `src/pm_lab_gen.h:60-62`).

### F2 — HIGH / usability — "flooded mid-section" is buried inside the floor slab

`tools/gen_lab_map.py:49` floor brush: Y `-20 .. 0` for the **full** `Z0..Z1`.
`tools/gen_lab_map.py:59` WATER: Y `-20 .. -6` for Z `260..520`.

The water AABB is strictly below the floor top. The player stands on Y=0 for the whole hall. `PM_SURF_WATER` never becomes the surface underfoot. The comment at `:56-58` ("shallow step down, so the player can hear themselves") is false in the geometry.

`kiln_map` reduces brushes to AABBs (`engine/src/kiln/kiln_map.h:17-24`) and does **not** yet key `PM_SURF_*` off the texture name (`tools/gen_lab_map.py:44-46`). Even after splitting the floor, surfaces stay DECK until that wiring exists.

### F3 — MEDIUM / durability — corridor `.map` has no regeneration gate

`tools/gen_lab_map.py` emits `assets/pm_lab.map`. There is **no** `nix/checks` entry (search of `nix/` is empty). `pm-gen-headers.nix` / `pm-rigs.nix` / `pm-demon-textures.nix` exist for every other generated artefact. Editing the Python and forgetting `--out` ships the old corridor with the new source looking applied — the exact failure `nix/checks/pm-rigs.nix:14-18` was written to catch.

`info_player_start` at `(0, 40, -40)` (`tools/gen_lab_map.py:79`) is ignored. `src/main.c:158` hardcodes `{{0, pm_lab_eye_height(), -40}}` with eye ≈ 104. If anyone later "fixes" spawn to honour the entity, Horner appears inside the floor.

### F4 — LOW / docs — three rooms, two of them named "the lab"

| Room | Draw | Clip | Player |
|---|---|---|---|
| PLAY corridor | `kiln_map` faces from `pm_lab.map` | same brushes | PLAY |
| Intro station | `dank_lab.t3dm` (`PM_MODEL_LAB`) | `LAB_BRUSHES` in `src/pm_lab.c:54-78` | LAB |
| `pm_world.py` `build_lab()` | nothing | nothing | unused (`src/pm_world_gen.h:45-50`) |

`docs/ASSET_PIPELINE.md:190-201` still says dank_lab / Horner / centaur CI4 are "not yet on this path". They are. Island tiling+LOD is correctly still out.

`src/main.c:155-157` says the corridor ceiling is 2.34 m. Interior is `CEIL_Y=130` → 130/64 = **2.03 m**; 2.34 m is the top of the 20-unit ceiling slab (`y=150`).

### F5 — YAGNI — island is not a walkable scene

`docs/ASSET_PIPELINE.md:199-201` and `tools/blender/pm_world.py:29-40`: the island is a hub mesh with flat field + six gates, authored so AABB brushes *could* match. PLAY never installs that clip world. Do not add `kiln_tile` / LOD in this pass.

### Open (not findings until measured)

- Demon spawn Y=10 (`gen_lab_map.py:81-91`) vs floor Y=0 vs first-person eye 104. `pm_veilreel.c:28-36` already records that a camera at eye height aims over their heads. Confirm feet vs origin on the t3dm before moving spawn Y.
- Overlord is `KILN_ACTOR_CAT_BOSS` so `kiln_target` (ENEMY..NPC) cannot lock it — combat, not geometry.

---

## Proposed approach

Fix PLAY water by **splitting the floor brush** in the generator (one source of truth), then gate the `.map` the same way rigs are gated. Fix LAB interactables by **deleting `MRI_POS` / `NOTES[]` literals** and deriving from `pm_lab_gen.h`. Do not merge the two labs. Do not author island collision.

---

## Step-by-step tasks

### Task 1: Record baseline (no code)

**Objective:** Know what is green before touching geometry.

**Step 1:** From `/home/asher/Documents/PetaByte-Madness`:

```bash
python3 tools/blender/test_world.py
python3 tools/gen_lab_map.py --out /tmp/pm_lab_regen.map && diff -u assets/pm_lab.map /tmp/pm_lab_regen.map
```

Expected: `test_world.py` PASS. `diff` empty (or list the drift — if non-empty, Task 3's first commit is "regen the map", not the water fix).

**Step 2:** Do not run `nix build .#petabyte-madness` on the laptop unless the user asks.

---

### Task 2: Failing test — water is a step, not a cavity

**Objective:** A bare-python test that is red on today's `BRUSHES`.

**Files:**
- Create: `tools/test_lab_map.py`

**Step 1: Write the failing test**

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Assert gen_lab_map.BRUSHES match the comments, without writing a file."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_lab_map as M

def boxes(tex):
    return [(mins, maxs) for mins, maxs, t in M.BRUSHES if t == tex]

def test_flood_is_the_floor_there():
    water = boxes("WATER")
    assert len(water) == 1, water
    (w0, w1) = water[0]
    # In Z 260..520 the highest DECK/WATER top must be the water top,
    # not the full-length floor at Y=0.
    z0, z1 = w0[2], w1[2]
    floor_tops = []
    for mins, maxs, tex in M.BRUSHES:
        if tex != "DECK":
            continue
        if maxs[2] <= z0 or mins[2] >= z1:
            continue
        # overlaps the flood span in Z
        floor_tops.append(maxs[1])
    assert floor_tops, "no DECK overlaps the flood span"
    assert max(floor_tops) <= w1[1] + 1e-6, (
        "floor still covers the flood at Y=%s; water top is %s" % (max(floor_tops), w1[1])
    )

def test_corridor_longer_than_veiled_far_shorter_than_cold():
    length = (M.Z1 + M.WALL) - (M.Z0 - M.WALL)
    assert length > 760, length
    assert length > 1000, length  # 1040 with end caps
    assert length < 1200, length

if __name__ == "__main__":
    test_flood_is_the_floor_there()
    test_corridor_longer_than_veiled_far_shorter_than_cold()
    print("ok")
```

**Step 2: Run to verify failure**

```bash
python3 tools/test_lab_map.py
```

Expected: FAIL — `floor still covers the flood at Y=0.0; water top is -6`

**Step 3: Split the floor in `tools/gen_lab_map.py`**

Replace the single shell floor entry (`tools/gen_lab_map.py:49`) with three spans. Keep WATER. Do not change HALF_W / Z0 / Z1 / WALL / plinths / entities.

```python
    # shell — floor SPLIT so the flood is a real step down (Y=-6), not a
    # box trapped inside the slab. kiln_clip is AABB: the highest top in a
    # column is what you stand on.
    ((-HALF_W, FLOOR_Y - WALL, Z0), (HALF_W, FLOOR_Y, 260), "DECK"),
    ((-HALF_W, FLOOR_Y - WALL, 520), (HALF_W, FLOOR_Y, Z1), "DECK"),
    ((-HALF_W, CEIL_Y, Z0), (HALF_W, CEIL_Y + WALL, Z1), "DECK"),
    # ... walls and end caps unchanged ...

    # flooded mid-section: walkable top at FLOOR_Y-6
    ((-HALF_W, FLOOR_Y - WALL, 260), (HALF_W, FLOOR_Y - 6, 520), "WATER"),
```

Delete the old full-length floor line. Keep the WATER line (now it is the walkable top in that span).

**Step 4: Re-run the test**

```bash
python3 tools/test_lab_map.py
```

Expected: `ok`

**Step 5: Emit the map and commit**

```bash
python3 tools/gen_lab_map.py --out assets/pm_lab.map
# expected stdout: assets/pm_lab.map: N brushes, 8 entities
# N is old brush count + 1 (one extra floor piece)
git add tools/gen_lab_map.py tools/test_lab_map.py assets/pm_lab.map
git commit -m "The flood is a step in the floor, not a box inside it"
```

Do not wire `PM_SURF_WATER` in `kiln_map` in this task (engine change, sibling repo). Track as follow-up.

---

### Task 3: Gate the corridor map

**Objective:** `nix/checks` fails if `assets/pm_lab.map` is stale vs `gen_lab_map.py`.

**Files:**
- Create: `nix/checks/pm-lab-map.nix`
- Modify: `flake.nix` `checks` attrset (~line 849)

**Step 1:** Mirror `nix/checks/pm-rigs.nix` shape: `pkgs.runCommand`, `python3`, regenerate into `$TMP`, `diff -u`. Also run `python3 tools/test_lab_map.py`.

Sketch:

```nix
# SPDX-License-Identifier: MPL-2.0
{ pkgs }:
pkgs.runCommand "check-pm-lab-map" {
  nativeBuildInputs = [ pkgs.python3 ];
} ''
  cp -r ${../../tools} tools
  cp -r ${../../assets} assets
  python3 tools/test_lab_map.py
  python3 tools/gen_lab_map.py --out regen.map
  diff -u assets/pm_lab.map regen.map
  touch $out
''
```

**Step 2:** Add `pm-lab-map = import ./nix/checks/pm-lab-map.nix { inherit pkgs; };` next to `pm-rigs` in `flake.nix`.

**Step 3:** `nix build .#checks.x86_64-linux.pm-lab-map` — **only this check**, not the ROM. Expected: `check-pm-lab-map` in the store, exit 0.

If the user forbids all nix on the laptop, run the two python commands from Task 2 instead and leave a comment in the check that CI is the first consumer.

**Step 4:** Commit `nix/checks/pm-lab-map.nix` and `flake.nix`.

```bash
git commit -m "Gate pm_lab.map the same way the rigs are gated"
```

---

### Task 4: MRI and notes derive from `pm_lab_gen.h`

**Objective:** USE on the scanner, notes on the desk.

**Files:**
- Modify: `src/pm_lab.c:141-217`
- Test: `tools/blender/test_world.py` does not cover C. Add a tiny native assertion in `nix/checks/pm-gen-headers.nix` **or** a comment-only compile-time check:

```c
/* After including pm_lab.h, MRI_POS must be the generator's mouth. */
```

Implement by deletion:

```c
// DELETE MRI_POS and NOTES literals.

static const struct { float x, y, z; uint8_t note; } NOTES[] = {
    { PM_DESK_X,      PM_DESK_SEAT_Y + 8.0f, PM_DESK_Z,      0 },
    { PM_DESK_X - 24, PM_DESK_SEAT_Y + 8.0f, PM_DESK_Z + 16, 1 },
    { PM_DESK_X + 18, PM_DESK_SEAT_Y + 8.0f, PM_DESK_Z - 12, 2 },
};

static const fm_vec3_t MRI_POS = {{
    PM_MRI_X,
    PM_LAB_Y0 + 40.0f,   /* use-volume centre, not bore axis */
    PM_MRI_Z_FACE,
}};
```

Keep `pm_lab_start_eye` derived from room constants if it is still a literal (`src/pm_lab.c:157-159` `{{60, EYE_H, 120}}`) — only change it if a jump ROM shows Horner inside a wall; do not retune cameras in the same commit as USE volumes.

**Verify (no ROM):**

```bash
rg -n 'MRI_POS|-330.0f' src/pm_lab.c
```

Expected: `MRI_POS` uses `PM_MRI_X`, no `-330`.

```bash
git add src/pm_lab.c
git commit -m "The MRI you USE is the MRI you see"
```

---

### Task 5: Spawn from the map or delete the entity

**Objective:** One spawn.

**Files:** `src/main.c:154-158`, `tools/gen_lab_map.py:79`

Pick **one**:

A. Honour `info_player_start` if `kiln_map` exposes it (read `kiln_map.h` / spawn list). Set entity origin to `(0, 0, -40)` (feet); add `pm_lab_eye_height()` in C. Change entity Y from 40 to 0 in the generator.

B. Delete `info_player_start` from `ENTITIES` and keep the C spawn. Document in `gen_lab_map.py` that PLAY spawn is `main.c`.

Prefer A if `KilnRoomSpawn` already has a player classname; else B (YAGNI).

Regen map, `python3 tools/test_lab_map.py`, commit.

---

### Task 6: Docs that lie about rooms

**Objective:** One paragraph so the next agent does not "unify" the labs.

**Files:** `README.md` remaining-work is stale on other topics — **do not** rewrite the whole README. Add 4 lines under Layout or Where this stands:

```
PLAY walks `assets/pm_lab.map` (corridor). The intro LAB walks `dank_lab`
collision in `src/pm_lab.c`, which is not that map and not `pm_world.py`'s
lab. The island is flyover/attract only.
```

Also fix `src/main.c:155-157` "2.34 m ceiling" → interior 2.03 m (`CEIL_Y/64`).

```bash
git add README.md src/main.c
git commit -m "PLAY, the intro lab, and the island are three different rooms"
```

---

### Task 7 (explicitly out of scope)

- Island `kiln_tile` / LOD
- Wiring `kiln_map` texture names → `PM_SURF_*` (engine, `~/Documents/M64`)
- Demon armature (`tools/blender/demonrig.py` is untracked leftover — do not merge it here)
- Overlord targeting (combat category, not brushes)

---

## Tests / validation

| After | Command | Expected |
|---|---|---|
| Task 2 | `python3 tools/test_lab_map.py` | `ok` |
| Task 2 | `python3 tools/gen_lab_map.py --out /tmp/m.map` | brush count = previous + 1 |
| Task 3 | `diff -u assets/pm_lab.map` vs regen | empty |
| Task 4 | `rg ' -330' src/pm_lab.c` | no match |
| Visual (user asked) | `./dev shot pm-play-cold play.png` **only if they allow a desktop shot** | player stands in water depression Z 260–520; MRI USE prompt on the scanner in LAB |

TDD per code task: red test → fix → green → commit.

---

## Risks, tradeoffs, open questions

- Splitting the floor changes PLAY collision. A player who currently walks "through" the flood at Y=0 will drop 6 units (9 cm) — that is the point. If `kiln_fpscam` cannot step down 6 units, raise the water top to `-2` or add a 6-unit ramp brush. Measure with the debug clip overlay (`clip N` in `pm_debug.c`) before retuning.
- Task 4 without a jump-ROM shot can still leave notes floating. `.#pm-jump-LAB` / `./dev shot` is the arbiter; the generator header is the constraint.
- `tools/blender/demonrig.py` is untracked from a still-running or dead Claude job. Do not `git add` it in these commits.
- Cheaper Grok for Tasks 2–3 and 6. Opus only if Task 4's desk placement fights the mesh in a capture.

---

## Execution handoff

Plan complete. Execute task-by-task in order (baseline → water test → map gate → MRI → spawn → docs). Report after each commit. Do not push.
