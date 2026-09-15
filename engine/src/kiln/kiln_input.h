/* SPDX-License-Identifier: MIT
 *
 * kiln_input.h — the engine's joypad wrapper. See CLAUDE.md's Phase C notes.
 *
 * ── One poll per frame, through a wrapper ──────────────────────────────
 * Every example before Phase C called libdragon's `joypad_poll` +
 * `joypad_get_inputs` directly at the top of its frame loop. That works but
 * scatters the deadzone, edge-detection and stick-normalisation logic across
 * every ROM that ever reads a controller. This module is the single place
 * those decisions live: `kiln_input_update()` polls once at the top of the
 * frame, and `kiln_input_get(port)` returns a deadzoned, edge-flagged view.
 *
 * ── Why a squared-magnitude deadzone, not a per-axis threshold ─────────
 * A per-axis threshold of "if |x| < 8, x = 0" makes the playable region a
 * square, so the stick reports motion along a diagonal at full magnitude at
 * the square's corners — the player walks when the stick is at rest on a
 * diagonal. A squared-magnitude check (`x*x + y*y < dz*dz`) makes the playable
 * region a disc, the shape of the actual mechanism. Costs one multiply and one
 * compare per axis; no sqrt, no atan2 — keeps the no-gratuitous-libm stance.
 *
 * ── Button edges, not just levels ───────────────────────────────────────
 * `buttons` is what's held this frame. `edges` is the subset that was NOT held
 * last frame (pressed-this-frame) — the bit you check for "tap A to lock on",
 * not "hold A to charge". `released` is the symmetric opposite. Held/edge/
 * released is the same triple libdragon's own `joypad_get_buttons_*` exposes;
 * consolidating it here means example code stops computing edges by hand with
 * XOR, which is what every prior example did.
 *
 * ── Stick normalisation ─────────────────────────────────────────────────
 * Raw N64 stick range is roughly ±85; we divide by `JOYPAD_RANGE_N64_STICK_MAX`
 * (90) so full-tilt reads as ~0.94, not 1.0. The 0.94 ceiling is intentional —
 * OEM sticks drift, and a stick that occasionally reports 100 would otherwise
 * produce unclamped 1.1 magnitudes that speed-blip the player. Clamp to [-1,1]
 * anyway. C-stick is exposed but not normalised here; the camera code that uses
 * it can apply its own curve.
 */
#ifndef KILN_INPUT_H
#define KILN_INPUT_H

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Button bitmask. Bit order matches libdragon's `joypad_buttons_t.raw` so
 *  `joypad_get_buttons(port).raw` maps directly onto these — no translation
 *  table. The named `JOYPAD_BUTTON_*` aren't used because they aren't stable
 *  across libdragon revisions; the bit positions are. */
enum {
    KILN_BTN_A     = 1 << 0,
    KILN_BTN_B     = 1 << 1,
    KILN_BTN_Z     = 1 << 2,
    KILN_BTN_START = 1 << 3,
    KILN_BTN_DU    = 1 << 4,
    KILN_BTN_DD    = 1 << 5,
    KILN_BTN_DL    = 1 << 6,
    KILN_BTN_DR    = 1 << 7,
    KILN_BTN_L     = 1 << 10,
    KILN_BTN_R     = 1 << 11,
    KILN_BTN_CU    = 1 << 12,
    KILN_BTN_CD    = 1 << 13,
    KILN_BTN_CL    = 1 << 14,
    KILN_BTN_CR    = 1 << 15,
};

typedef struct {
    float    stick_x;   /**< deadzoned, normalised to [-1, 1], right = +      */
    float    stick_y;   /**< deadzoned, normalised to [-1, 1], up    = +      */
    float    cstick_x;  /**< raw / RANGE_N64_STICK_MAX; not deadzoned         */
    float    cstick_y;  /**< raw / RANGE_N64_STICK_MAX; not deadzoned         */
    uint32_t buttons;   /**< held this frame (KILN_BTN_*)                       */
    uint32_t edges;     /**< pressed this frame (was up last frame)            */
    uint32_t released;  /**< released this frame (was down last frame)         */
} KilnInput;

/** Call `joypad_init` first (it is NOT called here — examples that don't read
 *  a controller shouldn't pay for the interrupt). This only zeroes state. */
void kiln_input_init(void);

/** Poll once per frame, at the top of the frame before any actor update.
 *  Computes edges by diffing against last frame's buttons. */
void kiln_input_update(void);

/** NULL → port 1 (JOYPAD_PORT_1). Returns a pointer to module-static state;
 *  do not cache across frames — always re-fetch in the frame you use it. */
const KilnInput *kiln_input_get(int port);

/** Convenience predicates. `port` is 1-based to match libdragon's
 *  JOYPAD_PORT_1..4; pass 0 for the default (port 1). */
int kiln_input_held    (int port, uint32_t mask);
int kiln_input_pressed (int port, uint32_t mask); /**< edge: was up, is down */
int kiln_input_released(int port, uint32_t mask);

/* ── Tapes: scripted input, for attract modes and jump ROMs ─────────────
 * A tape replaces a port's RAW pad state — buttons and stick counts — before
 * the deadzone and edge code above run, so a scripted press produces a real
 * edge on its first frame and a real release after, exactly as a thumb would.
 * Nothing downstream can tell, which is the point: an attract mode exercises
 * the same player code a person does.
 *
 * Two ways to run one, and they differ only in who wins:
 *   kiln_input_play        FORCED. The tape drives the port and the physical
 *                          pad is ignored. For jump ROMs: `./dev shot` has no
 *                          input path, and a build that must reach a state
 *                          cannot depend on `./dev drive`'s uinput chain.
 *   kiln_input_set_attract STANDBY. After `idle_frames` consecutive frames of
 *                          no buttons and a centred stick, the tape takes over
 *                          from its start; any real input hands the port back
 *                          on that same frame and restarts the idle count.
 *
 * A ROM that installs neither behaves exactly as before. */

/** One key: from `frame` on, the port reads these values until the next key.
 *  `buttons` is KILN_BTN_*; sticks are raw counts (±85 is full tilt). */
typedef struct {
    uint16_t frame;
    uint16_t buttons;
    int8_t   sx, sy;
    int8_t   cx, cy;
} KilnInputKey;

/** Keys in ascending `frame` order. The LAST key marks the end of the tape: a
 *  looping tape jumps to `loop_frame` on reaching it (the last key's own values
 *  are never applied), and a tape with `loop_frame == KILN_INPUT_NO_LOOP` holds
 *  the last key's values forever. Before the first key the port reads idle. */
typedef struct {
    const KilnInputKey *keys;
    uint16_t            count;
    uint16_t            loop_frame;
} KilnInputTape;

#define KILN_INPUT_NO_LOOP 0xFFFF

/** Drive `port` from `tape` starting next update, ignoring the pad. NULL stops.
 *  The tape is referenced, not copied — keep it static. */
void kiln_input_play(int port, const KilnInputTape *tape);

/** Arm `tape` to take over `port` after `idle_frames` idle updates. NULL
 *  disarms. A forced tape on the same port takes precedence. */
void kiln_input_set_attract(int port, const KilnInputTape *tape, uint16_t idle_frames);

/** 1 while a tape (forced or attract) is driving `port` — for a HUD badge, so
 *  a viewer can tell a demo playing itself from a stuck controller. */
int kiln_input_scripted(int port);

#ifdef __cplusplus
}
#endif

#endif /* KILN_INPUT_H */