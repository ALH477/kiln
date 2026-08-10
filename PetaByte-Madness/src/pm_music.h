// SPDX-License-Identifier: MPL-2.0
//
// pm_music.h — the main theme, in both of its forms.
//
// ── Two masters of one piece, and why both ship ────────────────────────
// The theme is a string quartet. It is in the ROM twice:
//
//   rom:/music/petabyte.xm64          2.5 KB   the SCORE
//   rom:/music/petabyte_stream.wav64  1.0 MB   the RECORDING
//
// The XM is the quartet as sequence data — four channels of notes driving
// four synthesised single-cycle instruments, converted from the authored
// MIDI by tools/midi_to_xm.py and played live by libdragon's RSP mixer.
// It costs almost nothing (libdragon benchmarks a 10-channel XM at "< 3%
// CPU and < 10% RSP") and loops exactly, because it is the score rather
// than a recording of one.
//
// The wav64 is the mastered mix, VADPCM-compressed and streamed from ROM.
// It is 400x the size and it is what the piece actually sounds like.
//
// The title sequence plays the score first and then the recording. That is
// not indecision — it is the game telling you what it is. You hear the
// console's own four voices playing the theme, and then you hear the thing
// they are an approximation of, which is the same joke the game makes
// about a man and the machine that scans him.
//
// ── Where it plays ─────────────────────────────────────────────────────
// The title, the attract reel and the file screen. It stops when the
// opening cinematic starts (PM_SCREEN_LAB_CINE), because from there the
// ambience bed on PM_CH_DRONE owns the mix and a tune under the intake
// scene would be the wrong kind of film.
//
// ── Missing assets are silent, not fatal ───────────────────────────────
// Same contract as pm_sfx: a ROM built without either file loads -1 and
// plays nothing. The game runs, the menus work, and the debug ROM says so
// in the log.

#ifndef PM_MUSIC_H
#define PM_MUSIC_H

/** Load both forms. Call once, after dfs_init and m64_audio_init. */
void pm_music_init(void);

/** Turn the theme on or off. Idempotent — call it every frame with the
 *  answer for the current screen and it starts, stops and hands over on
 *  its own. Turning it on always restarts from the score. */
void pm_music_set_active(int active);

/** One frame. `vol` is the master level for the theme, 0..1 — pass the
 *  screen fade in so the music ducks with the picture. Drives the handover
 *  from the score to the recording, and re-triggers the recording when it
 *  ends (a wav64 has no loop point baked in, so the loop is here). */
void pm_music_update(float dt, float vol);

/** Stop everything now. */
void pm_music_stop(void);

/** For the debug overlay: 0 silent, 1 playing the score, 2 playing the
 *  recording, -1 nothing loaded. */
int pm_music_state(void);

#endif // PM_MUSIC_H
