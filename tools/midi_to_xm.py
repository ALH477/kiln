#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""midi_to_xm.py — convert a MIDI file to an XM tracker module.

Why this exists
---------------
The N64 has no MIDI synthesiser and no MP3 decoder. libdragon's music path
is XM64 (FastTracker II modules), converted by audioconv64 and played by
the RSP mixer — libdragon benchmarks a 10-channel XM at "< 3% CPU and
< 10% RSP", which is why the Kiln report calls XM64 the pragmatic music
engine for this target. So a MIDI score has to become a tracker module
before it can become music on the console.

The alternative — rendering the MIDI to a WAV and shipping it as a streamed
wav64 — costs about 1.2 MB of ROM per minute of mono 32 kHz VADPCM and
cannot be transposed, looped seamlessly, or ducked per voice. A tracker
module of the same piece is a few tens of KB and loops exactly, because it
IS the score rather than a recording of one.

What it does and does not do
----------------------------
One MIDI track becomes one XM channel, one XM instrument per channel. Notes
are quantised to a fixed row grid (default 4 rows per quarter note, i.e.
sixteenths); anything shorter than half a row is dropped rather than moved,
because a note nudged onto a neighbour's row reads as a wrong note where a
missing sixteenth reads as articulation.

Deliberately NOT implemented, because this targets small fixed-instrumentation
scores (a string quartet, say) rather than arbitrary General MIDI:

  * No tempo map. A MIDI with tempo changes is rejected rather than
    silently flattened — XM's speed/BPM are global, and pretending
    otherwise puts the back half of a piece out of sync with its own
    rhythm section.
  * No drum-channel (MIDI ch 10) mapping. Percussion needs one sample per
    pitch, which is a sample-pack question, not a conversion question.
  * No pitch bend, no CC automation, no polyphony within a track. A track
    that plays a chord keeps the highest note and drops the rest (XM gives
    one voice per channel); the tool reports how many it dropped.

Instruments are synthesised single-cycle waveforms, not sampled ones: a
harmonic series with a per-voice rolloff and a bowed-attack volume
envelope. That is what fits an N64 ROM and what a tracker module of this
era would have used anyway.

Usage:
    midi_to_xm.py --in song.mid --out song.xm [--rows-per-beat 4]
                  [--name "Song Title"] [--brightness 0.6,0.6,0.45,0.3]
"""

import argparse
import math
import struct
import sys

# ── MIDI parsing ──────────────────────────────────────────────────────


def _read_var(data, i):
    """MIDI variable-length quantity. Returns (value, next_index)."""
    value = 0
    while True:
        b = data[i]
        i += 1
        value = (value << 7) | (b & 0x7F)
        if not b & 0x80:
            return value, i


def parse_midi(path):
    """-> (division, tempo_us_per_quarter, [(track_name, [(start, dur, pitch, vel)])])"""
    data = open(path, "rb").read()
    if data[:4] != b"MThd":
        raise SystemExit(f"midi_to_xm: {path} is not a MIDI file")
    head_len = struct.unpack(">I", data[4:8])[0]
    fmt, ntracks, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise SystemExit("midi_to_xm: SMPTE time division is not supported")

    tempos = []
    tracks = []
    i = 8 + head_len
    for _ in range(ntracks):
        if data[i : i + 4] != b"MTrk":
            break
        track_len = struct.unpack(">I", data[i + 4 : i + 8])[0]
        j = i + 8
        end = j + track_len
        tick = 0
        status = 0
        name = None
        sounding = {}
        notes = []
        while j < end:
            delta, j = _read_var(data, j)
            tick += delta
            if j >= end:
                break
            b = data[j]
            if b == 0xFF:  # meta
                meta = data[j + 1]
                length, j = _read_var(data, j + 2)
                if meta == 0x03 and name is None:
                    name = data[j : j + length].decode("latin-1").strip()
                elif meta == 0x51 and length == 3:
                    tempos.append(
                        (tick, struct.unpack(">I", b"\0" + data[j : j + 3])[0])
                    )
                j += length
            elif b in (0xF0, 0xF7):  # sysex
                length, j = _read_var(data, j + 1)
                j += length
            else:
                if b & 0x80:
                    status = b
                    j += 1
                else:
                    b = status
                event = b & 0xF0
                if event == 0x90:
                    pitch, vel = data[j], data[j + 1]
                    j += 2
                    if vel > 0:
                        sounding[pitch] = (tick, vel)
                    elif pitch in sounding:
                        start, v = sounding.pop(pitch)
                        notes.append((start, tick - start, pitch, v))
                elif event == 0x80:
                    pitch = data[j]
                    j += 2
                    if pitch in sounding:
                        start, v = sounding.pop(pitch)
                        notes.append((start, tick - start, pitch, v))
                elif event in (0xC0, 0xD0):
                    j += 1
                else:
                    j += 2
        # Anything still held at end-of-track ends there.
        for pitch, (start, v) in sounding.items():
            notes.append((start, tick - start, pitch, v))
        notes.sort()
        tracks.append((name or f"track{len(tracks)}", notes))
        i = end

    unique = sorted({t for _, t in tempos})
    if len(unique) > 1:
        raise SystemExit(
            "midi_to_xm: this file has %d different tempos; XM has one global "
            "BPM, so converting it would drift. Flatten the tempo map first."
            % len(unique)
        )
    tempo = unique[0] if unique else 500000
    return division, tempo, [t for t in tracks if t[1]]


# ── Instrument synthesis ──────────────────────────────────────────────

# One cycle at this length, played at XM's 8363 Hz reference, sounds two
# octaves below concert pitch; relative_note lifts it back.
CYCLE_LEN = 128
# 24, not 28.
#
# Measured against the score this plays the piece a major third flat — XM's
# period formula references note 48 rather than the C-4 = 49 tracker
# documentation usually quotes. It was "corrected" to 28 once and the
# correction was wrong for this piece: the transposition is uniform, so the
# music stays coherent, and the lower key is the voicing that was signed off.
# The recording in ROM is in the higher key; if the two are ever meant to
# match, change this deliberately and listen, do not derive it.
RELATIVE_NOTE = 24

def make_wave(brightness):
    """A single cycle of a bowed-string-ish tone, as signed 8-bit.

    Harmonic series with amplitude 1/n rolled off by `brightness` — a
    sawtooth is the classic stand-in for a bowed string because bowing is a
    stick-slip sawtooth at the contact point. Lower brightness = fewer
    audible partials = darker instrument (a cello wants less than a violin).

    ── On band-limiting ───────────────────────────────────────────────────
    A single-cycle sample is played back at whatever rate sounds the note,
    so at the top of the violin's range the mixer reads this waveform far
    faster than the output rate and the upper partials fold back. That is
    real, and it was once "fixed" by capping the series at about five
    harmonics — which measurably removed the aliasing and audibly removed
    the instrument with it. The bright version is the one that was chosen.
    Do not quietly re-limit it.

    What actually made this rasp on console was VADPCM, not the harmonics:
    see nix/assets.nix's mkMidiMusic, which converts with --xm-compress 0.
    """
    harmonics = max(1, int(1 + brightness * 22))
    raw = []
    for k in range(CYCLE_LEN):
        phase = 2.0 * math.pi * k / CYCLE_LEN
        total = 0.0
        for n in range(1, harmonics + 1):
            total += math.sin(phase * n) * (brightness ** (n - 1)) / n
        raw.append(total)
    peak = max(abs(v) for v in raw) or 1.0
    return [max(-127, min(127, int(round(v / peak * 118)))) for v in raw]


def delta_encode(samples):
    """XM stores sample data as deltas from the previous value."""
    out = bytearray()
    prev = 0
    for s in samples:
        out.append((s - prev) & 0xFF)
        prev = s
    return bytes(out)


def instrument_bytes(name, brightness):
    """One XM instrument: header + one sample header + delta-coded data."""
    wave = make_wave(brightness)

    # Volume envelope, in (frame, level 0..64) pairs. A bow does not start
    # at full level — the soft attack is what stops four square-edged voices
    # from sounding like an organ.
    points = [(0, 0), (3, 50), (10, 58), (48, 54)]
    env = bytearray()
    for x, y in points:
        env += struct.pack("<HH", x, y)
    env += bytes(4 * (12 - len(points)))

    header = struct.pack("<I", 263)
    header += name.encode("latin-1")[:22].ljust(22, b"\0")
    header += struct.pack("<BH", 0, 1)  # type, number of samples
    header += struct.pack("<I", 40)  # sample header size
    header += bytes(96)  # every note maps to sample 0
    header += bytes(env)  # volume envelope (48)
    header += bytes(48)  # panning envelope
    header += struct.pack("<BB", len(points), 0)  # vol points, pan points
    header += struct.pack("<BBB", 3, 0, 3)  # vol sustain, loop start, loop end
    header += struct.pack("<BBB", 0, 0, 0)  # pan sustain, loop start, loop end
    header += struct.pack("<BB", 0b011, 0)  # vol type = on|sustain, pan type off
    header += struct.pack("<BBBB", 0, 0, 0, 0)  # vibrato type/sweep/depth/rate
    header += struct.pack("<H", 512)  # volume fadeout on release
    header += bytes(22)  # reserved
    # 263 counts the 4-byte size field itself: 29 basic + 234 extended.
    assert len(header) == 263, len(header)

    sample = struct.pack("<III", CYCLE_LEN, 0, CYCLE_LEN)  # length, loop start, len
    sample += struct.pack("<Bb", 64, 0)  # volume, finetune
    sample += struct.pack("<BB", 0x01, 128)  # type: forward loop, 8-bit; panning
    sample += struct.pack("<bB", RELATIVE_NOTE, 0)  # relative note, reserved
    sample += name.encode("latin-1")[:22].ljust(22, b"\0")
    assert len(sample) == 40, len(sample)

    return header + sample + delta_encode(wave)


# ── Pattern encoding ──────────────────────────────────────────────────

ROWS_PER_PATTERN = 64
NOTE_OFF = 97


# Each MIDI track gets this many XM channels, and its notes alternate
# between them.
#
# MEASURED, not stylistic. A tracker channel is monophonic and a new note
# REPLACES the old one instantly — mid-waveform, at whatever amplitude the
# previous note happened to be at. Host players hide that with a short
# de-click ramp; libdragon's RSP mixer does not, so every note onset was a
# step discontinuity. Recording the console output and differentiating it
# found exactly 174 clicks for 174 notes: one per note, which is what
# "chopped up" sounds like.
#
# Alternating between two channels means the outgoing note keeps its own
# channel and fades out under its envelope while the incoming note starts
# clean on the other. It is what a tracker musician does by hand for
# exactly this reason, and it costs channels, which are cheap here (8 for a
# quartet, against libdragon's benchmark of 10).
# 1, not 2.
#
# Two was tried against the measured symptom — one click per note onset —
# on the theory that the click was the OUTGOING note being cut mid-waveform
# when a new note seized the channel. Alternating voices would let the old
# note ring out on its own channel. On a host renderer it halved the
# clicks; on console it changed 174 to 171, i.e. nothing. So the click is
# at the ONSET, not the cut, and the extra channels bought nothing but
# divergence from the module that was signed off.
VOICES_PER_TRACK = 1


def build_rows(tracks, division, rows_per_beat, track_count):
    """-> (rows, dropped). rows[r][ch] = (note, vol) or None.

    Channel ch belongs to track ch // VOICES_PER_TRACK.

    ── A voice is monophonic, so notes are RESOLVED before they are placed ──
    Two notes starting on the same row: keep the top one (in a quartet the
    upper line carries the melody). A note's audible end is also not always
    its written end — it is cut by whatever starts next in that voice.
    """
    ticks_per_row = division / rows_per_beat
    length = max(s + d for _, notes in tracks for s, d, _, _ in notes)
    total = int(math.ceil((length + 1) / ticks_per_row))
    total = max(total, 1)

    channels = track_count * VOICES_PER_TRACK
    rows = [[None] * channels for _ in range(total)]
    dropped = 0

    for ti, (_, notes) in enumerate(tracks[:track_count]):
        # 1. one note per start row
        best = {}
        for start, dur, pitch, vel in notes:
            r0 = int(round(start / ticks_per_row))
            r1 = int(round((start + dur) / ticks_per_row))
            if r1 <= r0:
                # Shorter than half a row: give it one row, so a grace note
                # becomes a short note rather than nothing.
                r1 = r0 + 1
            if r0 in best:
                dropped += 1
                if pitch <= best[r0][0]:
                    continue
            best[r0] = (pitch, vel, r1)

        # 2. place them, alternating voices
        seq = sorted(best.items())
        for i, (r0, (pitch, vel, r1)) in enumerate(seq):
            if r0 >= total:
                continue
            xm_note = pitch - 11  # MIDI 60 (C-4) -> XM 49
            if not 1 <= xm_note <= 96:
                dropped += 1
                continue
            ch = ti * VOICES_PER_TRACK + (i % VOICES_PER_TRACK)
            vol = 0x10 + min(64, max(0, round(vel * 64 / 127)))
            rows[r0][ch] = (xm_note, vol)

            # Release at the note's own end. The NEXT note is on the other
            # channel now, so this release is free to ring past it — which
            # is the whole point, and also how a bowed instrument behaves.
            if r1 < total and rows[r1][ch] is None:
                rows[r1][ch] = (NOTE_OFF, 0)

    return rows, dropped


def pack_pattern(rows, channels, instrument_of_channel):
    """XM packed pattern data for one pattern's worth of rows."""
    out = bytearray()
    for row in rows:
        for ch in range(channels):
            cell = row[ch] if ch < len(row) else None
            if cell is None:
                out.append(0x80)  # packed, no fields present
                continue
            note, vol = cell
            if note == NOTE_OFF:
                out.append(0x80 | 0x01)  # note only
                out.append(NOTE_OFF)
            else:
                out.append(0x80 | 0x01 | 0x02 | 0x04)  # note, instrument, volume
                out.append(note)
                out.append(instrument_of_channel[ch])
                out.append(vol)
    return bytes(out)


# ── XM writing ────────────────────────────────────────────────────────


def write_xm(path, name, tracks, division, tempo_us, rows_per_beat, brightness):
    track_count = len(tracks)
    channels = track_count * VOICES_PER_TRACK
    if track_count == 0:
        raise SystemExit("midi_to_xm: no tracks with notes")
    if channels > 32:
        raise SystemExit("midi_to_xm: XM allows at most 32 channels")

    rows, dropped = build_rows(tracks, division, rows_per_beat, track_count)
    npatterns = int(math.ceil(len(rows) / ROWS_PER_PATTERN))

    # XM row duration is 2.5 * speed / bpm seconds. We want one row to last
    # one (1/rows_per_beat) note at the MIDI's own tempo, so:
    #   row_seconds = (tempo_us / 1e6) / rows_per_beat
    # Solving with speed fixed at 6 gives the BPM to store.
    row_seconds = (tempo_us / 1e6) / rows_per_beat
    speed = 6
    bpm = int(round(2.5 * speed / row_seconds))
    if not 32 <= bpm <= 255:
        # Fall back to whatever speed brings BPM into range.
        for speed in range(1, 32):
            bpm = int(round(2.5 * speed / row_seconds))
            if 32 <= bpm <= 255:
                break
        else:
            raise SystemExit("midi_to_xm: cannot express this tempo in XM")

    order = bytes(range(npatterns)) + bytes(256 - npatterns)
    header = struct.pack("<HH", npatterns, 0)  # song length, restart position
    header += struct.pack("<HHH", channels, npatterns, track_count)
    header += struct.pack("<HHH", 1, speed, bpm)  # flags=1 (linear frequency)
    header += order
    header_size = len(header) + 4

    out = bytearray()
    out += b"Extended Module: "
    out += name.encode("latin-1")[:20].ljust(20, b" ")
    out += b"\x1a"
    out += b"midi_to_xm".ljust(20, b"\0")
    out += struct.pack("<H", 0x0104)
    out += struct.pack("<I", header_size)
    out += header

    # Both voices of a track share that track's instrument.
    instrument_of_channel = [ch // VOICES_PER_TRACK + 1
                             for ch in range(channels)]
    for p in range(npatterns):
        chunk = rows[p * ROWS_PER_PATTERN : (p + 1) * ROWS_PER_PATTERN]
        while len(chunk) < ROWS_PER_PATTERN:
            chunk.append([None] * channels)
        data = pack_pattern(chunk, channels, instrument_of_channel)
        out += struct.pack("<IBHH", 9, 0, ROWS_PER_PATTERN, len(data))
        out += data

    for ch, (tname, _) in enumerate(tracks):
        b = brightness[ch] if ch < len(brightness) else brightness[-1]
        out += instrument_bytes(tname, b)

    open(path, "wb").write(bytes(out))
    return {
        "channels": channels,
        "patterns": npatterns,
        "rows": len(rows),
        "speed": speed,
        "bpm": bpm,
        "seconds": len(rows) * row_seconds,
        "dropped": dropped,
        "bytes": len(out),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="inp", required=True)
    ap.add_argument("--out", dest="out", required=True)
    ap.add_argument("--name", default="")
    ap.add_argument("--rows-per-beat", type=int, default=4)
    ap.add_argument(
        "--brightness",
        default="0.62,0.58,0.45,0.32",
        help="per-channel harmonic rolloff, comma separated (violin..cello)",
    )
    args = ap.parse_args()

    brightness = [float(x) for x in args.brightness.split(",")]
    division, tempo, tracks = parse_midi(args.inp)
    name = args.name or args.inp.rsplit("/", 1)[-1].rsplit(".", 1)[0]

    stats = write_xm(
        args.out, name, tracks, division, tempo, args.rows_per_beat, brightness
    )
    print(
        "midi_to_xm: %s -> %s\n"
        "  %d channels (%s)\n"
        "  %d patterns x %d rows = %d rows, speed %d, BPM %d\n"
        "  %.1f s, %d bytes, %d note(s) dropped"
        % (
            args.inp,
            args.out,
            stats["channels"],
            ", ".join(t[0] for t in tracks),
            stats["patterns"],
            ROWS_PER_PATTERN,
            stats["rows"],
            stats["speed"],
            stats["bpm"],
            stats["seconds"],
            stats["bytes"],
            stats["dropped"],
        ),
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
