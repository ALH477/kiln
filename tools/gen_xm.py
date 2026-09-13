#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate examples/music/test.xm: a short, looping, four-channel XM.

usage: gen_xm.py [out.xm]

Lead arpeggio, bass, a slow counter-line and a noise hat, two 16-row patterns
in a loop, 125 BPM at speed 6. Every instrument is a tiny synthesised sample,
so the file stays around 2 KB.

── Why this was rewritten ─────────────────────────────────────────────
The previous generator wrote a file that parsed as an EMPTY module:
audioconv64 converted it to "ctx:0, patterns:0, samples:0", and the music
example booted in Ares reporting 0 channels and a flat scope while its HUD
said PLAY. Three layout errors, each enough on its own:
  * no 0x1A byte between the song and tracker names, so every field after
    offset 37 was read one byte early;
  * header_size left out its own four bytes and was written twice;
  * the instrument header size left out its own four bytes and the sample
    header size inside it was zero.
The layout below follows tools/midi_to_xm.py, whose output has been played
on console, and verify() reads the fields back before the file is written.
"""
import math
import struct
import sys

CYCLE = 32          # samples per cycle; C-4 plays at 8363 Hz -> 261 Hz
NOTE_OFF = 97
ROWS = 16
SPEED, BPM = 6, 125
CHANNELS = 4
NAMES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]


def note(n):
    """'C-4' -> XM note number (1 = C-0). Kept at or below B-5: at CYCLE 32 a
    B-5 plays its sample at ~31.6 kHz, under libdragon's default per-channel
    mixer limit of the output rate (32 kHz), which the mixer asserts on."""
    v = NAMES.index(n[:2]) + 12 * int(n[2]) + 1
    assert v <= NAMES.index("B-") + 12 * 5 + 1, n
    return v


def delta(samples):
    out, prev = bytearray(), 0
    for s in samples:
        out.append((s - prev) & 0xFF)
        prev = s
    return bytes(out)


def wave_pulse():
    return [96 if k < CYCLE // 4 else -96 for k in range(CYCLE)]


def wave_saw():
    return [int(round(-110 + 220 * k / (CYCLE - 1))) for k in range(CYCLE)]


def wave_triangle():
    return [int(round(110 * (1 - 4 * abs(k / CYCLE - 0.5)))) for k in range(CYCLE)]


def wave_noise(n=1024):
    x, out = 0x1234, []
    for _ in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(((x >> 16) & 0xFF) - 128)
    return out


def instrument(name, samples, loop, env, sustain):
    """One instrument, one sample. `env` is [(frame, level 0..64)]."""
    e = bytearray()
    for x, y in env:
        e += struct.pack("<HH", x, y)
    e += bytes(4 * (12 - len(env)))

    h = struct.pack("<I", 263)                       # counts itself: 29 + 234
    h += name.encode("latin-1")[:22].ljust(22, b"\0")
    h += struct.pack("<BH", 0, 1)                    # type, one sample
    h += struct.pack("<I", 40)                       # sample header size
    h += bytes(96)                                   # all notes -> sample 0
    h += bytes(e) + bytes(48)                        # volume, panning envelopes
    h += struct.pack("<BB", len(env), 0)
    h += struct.pack("<BBB", sustain if sustain is not None else 0, 0, 0)
    h += struct.pack("<BBB", 0, 0, 0)
    h += struct.pack("<BB", 0b001 | (0b010 if sustain is not None else 0), 0)
    h += struct.pack("<BBBB", 0, 0, 0, 0)            # vibrato
    h += struct.pack("<H", 1024)                     # fadeout on key-off
    h += bytes(22)
    assert len(h) == 263, len(h)

    n = len(samples)
    s = struct.pack("<III", n, 0, n if loop else 0)
    s += struct.pack("<Bb", 64, 0)
    s += struct.pack("<BB", 0x01 if loop else 0x00, 128)   # 8-bit; panning
    s += struct.pack("<bB", 0, 0)                           # relative note
    s += name.encode("latin-1")[:22].ljust(22, b"\0")
    assert len(s) == 40, len(s)
    return h + s + delta(samples)


# rows: per channel, 16 entries of None, a note name, or "off".
# Instruments: 1 lead, 2 bass, 3 counter, 4 hat.
PATTERNS = [
    {
        0: ["C-5", None, "E-5", None, "G-5", None, "E-5", None,
            "C-5", None, "E-5", None, "G-5", None, "A-5", None],
        1: ["C-3", None, None, None, "C-3", None, "G-2", None,
            "C-3", None, None, None, "G-2", None, "off", None],
        2: ["E-4", None, None, None, None, None, None, None,
            "G-4", None, None, None, None, None, None, None],
        3: ["C-4", None, "C-4", None, "C-4", None, "C-4", None,
            "C-4", None, "C-4", None, "C-4", None, "C-4", "C-4"],
    },
    {
        0: ["A-4", None, "C-5", None, "E-5", None, "C-5", None,
            "F-4", None, "A-4", None, "C-5", None, "B-4", None],
        1: ["A-2", None, None, None, "A-2", None, "E-2", None,
            "F-2", None, None, None, "G-2", None, "off", None],
        2: ["C-4", None, None, None, None, None, None, None,
            "D-4", None, None, None, None, None, None, None],
        3: ["C-4", None, "C-4", None, "C-4", None, "C-4", None,
            "C-4", None, "C-4", None, "C-4", "C-4", "C-4", "C-4"],
    },
]
CHANNEL_VOL = [44, 56, 36, 20]   # volume column level per channel, 0..64


def pack(pattern):
    out = bytearray()
    for row in range(ROWS):
        for ch in range(CHANNELS):
            cell = pattern[ch][row]
            if cell is None:
                out.append(0x80)
            elif cell == "off":
                out += bytes([0x80 | 0x01, NOTE_OFF])
            else:
                vol = CHANNEL_VOL[ch] + (8 if ch == 3 and row % 4 == 0 else 0)
                out += bytes([0x80 | 0x01 | 0x02 | 0x04, note(cell), ch + 1, 0x10 + min(vol, 64)])
    return bytes(out)


def build():
    npat = len(PATTERNS)
    header = struct.pack("<HH", npat, 0)                  # song length, restart
    header += struct.pack("<HHH", CHANNELS, npat, 4)       # channels, patterns, instruments
    header += struct.pack("<HHH", 1, SPEED, BPM)           # linear frequencies
    header += bytes(range(npat)) + bytes(256 - npat)       # order table

    out = bytearray()
    out += b"Extended Module: "
    out += b"kiln test".ljust(20, b" ")
    out += b"\x1a"
    out += b"gen_xm.py".ljust(20, b"\0")
    out += struct.pack("<H", 0x0104)
    out += struct.pack("<I", len(header) + 4)              # counts itself
    out += header
    for p in PATTERNS:
        data = pack(p)
        out += struct.pack("<IBHH", 9, 0, ROWS, len(data)) + data
    out += instrument("lead", wave_pulse(), True, [(0, 64), (6, 40), (24, 30)], 2)
    out += instrument("bass", wave_saw(), True, [(0, 64), (12, 48)], 1)
    out += instrument("counter", wave_triangle(), True, [(0, 0), (6, 50), (40, 40)], 2)
    out += instrument("hat", wave_noise(), False, [(0, 64), (2, 18), (6, 0)], None)
    return bytes(out)


def verify(xm):
    """Read back the fields a loader reads, at the offsets it reads them."""
    assert xm[:17] == b"Extended Module: "
    assert xm[37] == 0x1A, "missing 0x1A at offset 37"
    assert struct.unpack_from("<H", xm, 58)[0] == 0x0104
    hsize = struct.unpack_from("<I", xm, 60)[0]
    assert hsize == 276, hsize
    songlen, restart, ch, npat, nins, flags, speed, bpm = struct.unpack_from("<HHHHHHHH", xm, 64)
    assert (ch, npat, nins, speed, bpm) == (CHANNELS, len(PATTERNS), 4, SPEED, BPM)
    off = 60 + hsize
    for _ in range(npat):
        plen, _, rows, dsize = struct.unpack_from("<IBHH", xm, off)
        assert plen == 9 and rows == ROWS
        off += plen + dsize
    for _ in range(nins):
        isize = struct.unpack_from("<I", xm, off)[0]
        nsmp = struct.unpack_from("<H", xm, off + 27)[0]
        shsize = struct.unpack_from("<I", xm, off + 29)[0]
        assert isize == 263 and nsmp == 1 and shsize == 40, (isize, nsmp, shsize)
        slen = struct.unpack_from("<I", xm, off + isize)[0]
        off += isize + shsize + slen
    assert off == len(xm), (off, len(xm))
    return ch, npat, nins


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test.xm"
    xm = build()
    ch, npat, nins = verify(xm)
    with open(out, "wb") as f:
        f.write(xm)
    print(f"gen_xm: {out}: {len(xm)} bytes, {ch} channels, {npat} patterns x {ROWS} rows, "
          f"{nins} instruments, {BPM} bpm")


if __name__ == "__main__":
    main()
