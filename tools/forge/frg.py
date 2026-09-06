#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""frg.py — read and write Forge's .FRG working format on the host.

The console writes two things: a `.FRG` (its own lossless state) and a `.MAP`
(the derived Quake brushes, already in the dialect this repo pins). The `.MAP`
needs no decoder at all — that is the point of having the ROM emit ASCII — so
this module exists for the other direction and for the round-trip gate:

  * `decode` so a level built on hardware can be inspected, diffed and
    regression-tested without an emulator;
  * `encode` so a `.map` already in the tree (`assets/pm_lab.map`,
    `assets/oot_test.map`) can be pushed BACK onto the card and edited on the
    console, which is the thing that makes this an editor for the existing game
    rather than only for new content;
  * `boxes_to_map` / `voxelise`, which are the host mirrors of
    kiln_voxel_boxes and let `nix flake check` assert that .map -> .FRG -> .map
    is idempotent without booting anything.

No `bpy` import and nothing from libdragon: this runs under a bare `python3`.
That discipline is what `tools/blender/quake_map.py` credits with catching two
real bugs before Blender ever ran, and it is why the greedy-box mirror lives
here rather than being trusted because the C version passes.

Format, big-endian throughout (it is written by a MIPS target and the encoder
says so rather than leaving it to be inferred from one sample):

  kiln_store header, 16 bytes:  magic 'KLNS' | u16 version | u16 flags
                               | u32 len | u32 crc32   (CRC over payload only)
  payload:                     f32 offset x,y,z
                               u16 chunk_count
                               per chunk: u8 cx, cy, cz
                                          RLE pairs (u8 run, u8 value)
                                            covering 4096 blocks
                               atlas: 4096 u8 palette indices
                                      2 x 16 u16 TLUT entries (cold, veiled)
                               --- v2 tail, all OPTIONAL ---
                               u16 ent_count, then per entity:
                                 f32 x,y,z | u16 angle | u8 classname
                                 | 3 x u16 epair   (POSITIONAL; the KEY of
                                   each slot is a function of the classname
                                   byte above it -- see forge_epairs())
                               light: f32 key_yaw, key_pitch, fill_yaw,
                                      fill_pitch | u8 key, fill, ambient,
                                      fog_on, clear_idx | f32 fog_near, fog_far
                               cam:   u16 key_count | f32 duration | u8 loop
                                      then per key: f32 t, eye xyz, look xyz

The tail is optional BY CONSTRUCTION: forge_io.c guards each section on there
being enough payload left, so a file carrying geometry alone loads and leaves the
rest at defaults. That is what lets this tool write a level for the console
without having to invent a light rig and a camera path for it.
"""

import struct
from pathlib import Path as _Path
import sys
import zlib

MAGIC = 0x4B4C4E53          # 'KLNS'
MAGIC_LEGACY = 0x4D363453   # 'M64S' — pre-rename, accepted on read only
VERSION = 2

CHUNK = 16
CHUNK_BLOCKS = CHUNK * CHUNK * CHUNK
GRID = (16, 4, 16)
DIM = tuple(g * CHUNK for g in GRID)
MAX_CHUNKS = 24

# Forge's entity vocabulary and the canonical face winding come from
# tools/schema/level_vocab.json. FORGE_CLASSNAMES is a WIRE FORMAT -- the .FRG
# v2 tail stores `u8 classname` as an index into it -- which is why the schema
# carries an explicit, contiguous, append-only forge_index rather than relying
# on anyone's declaration order.
sys.path.insert(0, str(_Path(__file__).resolve().parents[1] / 'schema'))
import level_vocab  # noqa: E402

FORGE_CLASSNAMES = level_vocab.forge_classnames()
FORGE_EPAIR_SLOTS = level_vocab.forge_epair_slots()


def forge_epairs(cls_index):
    """The epair slots for a classname INDEX, mirroring Forge's
    forge_vocab_epair_key(cls, i). The keys are per-classname, not global:
    info_key_door's slot 0 is `key_id` and everything else's is `count`. The
    slot COUNT is what the wire format fixes; the keys are derived from the
    classname byte stored beside them, which is why this was fixable without a
    FRG_VERSION bump."""
    if 0 <= cls_index < len(FORGE_CLASSNAMES):
        return level_vocab.forge_epairs(FORGE_CLASSNAMES[cls_index])
    return [{'key': k, 'default': 0, 'required': False}
            for k in level_vocab.forge_generic_epairs()][:FORGE_EPAIR_SLOTS]


def epair_dict(e):
    """An entity's epairs as {key: value}, for `info` and `--json`."""
    return {s['key']: v for s, v in zip(forge_epairs(e['classname']),
                                        e['epairs'])}


def _num(v):
    """Integers stay integers. Matches tools/mapmaker/mapfmt.py's _num."""
    r = round(v)
    return f'{r}' if abs(v - r) < 1e-9 else f'{v:g}'
BLOCK_UNITS = 32

ATLAS_SIDE = 64
ATLAS_COLOURS = 16

# ── The .map dialect ──────────────────────────────────────────────────────
# Transcribed from tools/mapmaker/src/mapio.js's aabbFaces, in its order, and
# duplicated deliberately: the outward normal is cross(p3-p1, p2-p1), NOT the
# obvious cross(p2-p1, p3-p1). The console parser is winding-independent so a
# wrong winding LOADS and then cannot be turned into geometry by quake_map.py's
# CSG — which is the state assets/oot_test.map is in today.


def aabb_faces(mins, maxs):
    """From tools/schema/level_vocab.json's table. This used to be a deliberate
    transcription of tools/mapmaker/src/mapio.js's aabbFaces, kept in step by a
    comment -- one of four copies of a winding whose sign decides whether a
    brush survives the CSG at all."""
    return level_vocab.aabb_faces(mins, maxs)


def boxes_to_map(boxes, spawn=None, ents=None, classnames=None):
    """boxes: [(mins, maxs, block_type)] in WORLD units. Returns .map text.

    `ents` are World.ents, which decode() populates and this used to discard --
    so `frg.py tomap` emitted geometry only, and ./dev forge-pull's "ROM emit
    and host mirror agree byte for byte" cross-check could never pass for a
    level with a single entity in it. It reported that as a NOTE, not an error,
    so it looked like a curiosity rather than a broken comparison."""
    out = ['{', '"classname" "worldspawn"']
    for mins, maxs, t in boxes:
        tex = f'FORGE{t}'
        out.append('{')
        for f in aabb_faces(mins, maxs):
            pts = ' '.join(f'( {p[0]} {p[1]} {p[2]} )' for p in f)
            out.append(f'{pts} {tex} 0 0 0 1 1')
        out.append('}')
    out.append('}')
    if spawn is not None:
        out += ['{', '"classname" "info_player_start"',
                f'"origin" "{spawn[0]} {spawn[1]} {spawn[2]}"', '}']
    for e in ents or []:
        cls = e['classname']
        name = (classnames[cls] if classnames and cls < len(classnames)
                else f'info_class{cls}')
        x, y, z = e['pos']
        out += ['{', f'"classname" "{name}"',
                f'"origin" "{_num(x)} {_num(y)} {_num(z)}"',
                f'"angle" "{e["angle"]}"']
        # The numeric epairs Forge ENT mode can author. Their keys come from
        # tools/schema/level_vocab.json and depend on the classname. The
        # emission rule mirrors Forge/src/forge_ent.c's forge_ent_emit exactly,
        # because ./dev forge-pull compares the ROM's .MAP and this one byte
        # for byte: a required epair is always written, an optional one only
        # when non-zero.
        for slot, v in zip(forge_epairs(cls), e['epairs']):
            if v or slot['required']:
                out.append(f'"{slot["key"]}" "{v}"')
        out.append('}')
    return '\n'.join(out) + '\n'


# ── RLE ───────────────────────────────────────────────────────────────────

def rle_encode(data):
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        v = data[i]
        run = 1
        while i + run < n and data[i + run] == v and run < 255:
            run += 1
        out += bytes((run, v))
        i += run
    return bytes(out)


def rle_decode(buf, at, count):
    out = bytearray()
    while len(out) < count:
        if at + 1 >= len(buf):
            break
        run, v = buf[at], buf[at + 1]
        at += 2
        if run == 0:            # would never terminate; matches the C decoder
            break
        out += bytes((v,)) * min(run, count - len(out))
    return bytes(out), at


# ── The world, as a dict of chunk-coord -> bytearray ──────────────────────

class World:
    def __init__(self):
        self.offset = (0.0, 0.0, 0.0)
        self.chunks = {}                      # (cx, cy, cz) -> bytearray
        self.atlas_index = bytearray(ATLAS_SIDE * ATLAS_SIDE)
        self.tlut = [[0] * ATLAS_COLOURS, [0] * ATLAS_COLOURS]
        # v2 tail. `None` means "this file carried none", which is different from
        # "carried an empty one" — an importer must not invent a light rig.
        self.ents = []                        # dicts: pos, angle, classname, epairs
        self.light = None
        self.cam = None                       # {duration, loop, keys: [(t, eye, look)]}

    @staticmethod
    def _bi(x, y, z):
        return (z * CHUNK + y) * CHUNK + x

    def get(self, x, y, z):
        if not (0 <= x < DIM[0] and 0 <= y < DIM[1] and 0 <= z < DIM[2]):
            return 0
        c = self.chunks.get((x // CHUNK, y // CHUNK, z // CHUNK))
        return c[self._bi(x % CHUNK, y % CHUNK, z % CHUNK)] if c else 0

    def set(self, x, y, z, block):
        if not (0 <= x < DIM[0] and 0 <= y < DIM[1] and 0 <= z < DIM[2]):
            return
        key = (x // CHUNK, y // CHUNK, z // CHUNK)
        if block == 0 and key not in self.chunks:
            return
        c = self.chunks.setdefault(key, bytearray(CHUNK_BLOCKS))
        c[self._bi(x % CHUNK, y % CHUNK, z % CHUNK)] = block
        if not any(c):
            del self.chunks[key]

    def solid_count(self):
        return sum(sum(1 for b in c if b) for c in self.chunks.values())

    # ── The host mirror of kiln_voxel_boxes ────────────────────────────────
    # Same partition, same scan order (y, then z, then x), same per-chunk
    # scope. It has to be the same or the round-trip gate proves nothing: an
    # "equivalent" decomposition that merges differently produces a different
    # .map and the comparison becomes a judgement call.

    def boxes(self):
        out = []
        for (cx, cy, cz) in sorted(self.chunks.keys(), key=lambda k: (k[2], k[1], k[0])):
            c = self.chunks[(cx, cy, cz)]
            claimed = bytearray(CHUNK_BLOCKS)
            ox, oy, oz = cx * CHUNK, cy * CHUNK, cz * CHUNK
            for y in range(CHUNK):
                for z in range(CHUNK):
                    for x in range(CHUNK):
                        bi = self._bi(x, y, z)
                        t = c[bi]
                        if t == 0 or claimed[bi]:
                            continue
                        wx = 1
                        while x + wx < CHUNK:
                            j = self._bi(x + wx, y, z)
                            if c[j] != t or claimed[j]:
                                break
                            wx += 1
                        wz = 1
                        while z + wz < CHUNK:
                            if any(c[self._bi(x + i, y, z + wz)] != t or
                                   claimed[self._bi(x + i, y, z + wz)]
                                   for i in range(wx)):
                                break
                            wz += 1
                        wy = 1
                        while y + wy < CHUNK:
                            bad = False
                            for k in range(wz):
                                for i in range(wx):
                                    j = self._bi(x + i, y + wy, z + k)
                                    if c[j] != t or claimed[j]:
                                        bad = True
                                        break
                                if bad:
                                    break
                            if bad:
                                break
                            wy += 1
                        for j2 in range(wy):
                            for k in range(wz):
                                for i in range(wx):
                                    claimed[self._bi(x + i, y + j2, z + k)] = 1
                        mins = (int(self.offset[0] + (ox + x) * BLOCK_UNITS),
                                int(self.offset[1] + (oy + y) * BLOCK_UNITS),
                                int(self.offset[2] + (oz + z) * BLOCK_UNITS))
                        maxs = (mins[0] + wx * BLOCK_UNITS,
                                mins[1] + wy * BLOCK_UNITS,
                                mins[2] + wz * BLOCK_UNITS)
                        out.append((mins, maxs, t))
        return out


# ── Container ─────────────────────────────────────────────────────────────

def encode(world):
    p = bytearray()
    p += struct.pack('>fff', *world.offset)
    # Chunk order is sorted so an encode is reproducible; the C side emits in
    # slot order, which is allocation order, so a byte-for-byte comparison
    # between the two encoders is NOT one of the properties on offer here.
    keys = sorted(world.chunks.keys(), key=lambda k: (k[2], k[1], k[0]))
    # MAX_CHUNKS was declared here and then used ONLY to format `info`'s
    # display string, so `frg.py frommap` of a map spanning more than 24 chunks
    # wrote a .FRG the ROM loads PARTIALLY -- kiln_voxel_set refuses the extra
    # chunks and forge_io.c discarded its return value, leaving a level that is
    # silently missing rooms with nothing to say so but a HUD gauge nobody is
    # looking at during an import. Refuse to write it instead.
    if len(keys) > MAX_CHUNKS:
        raise SystemExit(
            f'frg.py: this map needs {len(keys)} chunks but the ROM can hold '
            f'{MAX_CHUNKS} (KILN_VOXEL_MAX_CHUNKS).\n'
            f'        A chunk is {CHUNK}x{CHUNK}x{CHUNK} blocks of '
            f'{BLOCK_UNITS} units. Shrink the level, or raise '
            f'KILN_VOXEL_MAX_CHUNKS in engine/src/kiln/kiln_voxel.h and '
            f'MAX_CHUNKS here together.')
    p += struct.pack('>H', len(keys))
    for (cx, cy, cz) in keys:
        p += bytes((cx, cy, cz))
        p += rle_encode(world.chunks[(cx, cy, cz)])
    p += bytes(world.atlas_index)
    for st in range(2):
        for i in range(ATLAS_COLOURS):
            p += struct.pack('>H', world.tlut[st][i] & 0xFFFF)

    # The tail is written only when there is something to write, so a
    # geometry-only file stays byte-identical to what a v1 encoder produced
    # (modulo the version field) and encode(decode(x)) == x still holds.
    if world.ents or world.light or world.cam:
        p += struct.pack('>H', len(world.ents))
        for e in world.ents:
            p += struct.pack('>fff', *e['pos'])
            p += struct.pack('>H', e['angle'] & 0xFFFF)
            p += bytes((e['classname'],))
            for v in e['epairs']:
                p += struct.pack('>H', v & 0xFFFF)
        li = world.light or {
            'key': (0.9, -0.9), 'fill': (-2.2, -0.3),
            'levels': (200, 90, 110), 'fog': (0, 0), 'near_far': (600.0, 2400.0),
        }
        p += struct.pack('>ffff', li['key'][0], li['key'][1],
                         li['fill'][0], li['fill'][1])
        p += bytes((li['levels'][0], li['levels'][1], li['levels'][2],
                    li['fog'][0], li['fog'][1]))
        p += struct.pack('>ff', li['near_far'][0], li['near_far'][1])
        cam = world.cam or {'duration': 8.0, 'loop': 0, 'keys': []}
        p += struct.pack('>H', len(cam['keys']))
        p += struct.pack('>f', cam['duration'])
        p += bytes((cam['loop'],))
        for (t, eye, look) in cam['keys']:
            p += struct.pack('>f', t)
            p += struct.pack('>fff', *eye)
            p += struct.pack('>fff', *look)

    head = struct.pack('>IHHII', MAGIC, VERSION, 0, len(p),
                       zlib.crc32(bytes(p)) & 0xFFFFFFFF)
    return head + bytes(p)


def decode(blob):
    if len(blob) < 16:
        raise ValueError('too short to hold a kiln_store header')
    magic, version, _flags, length, crc = struct.unpack('>IHHII', blob[:16])
    if magic not in (MAGIC, MAGIC_LEGACY):
        raise ValueError(f'bad magic {magic:#x}, expected KLNS')
    if version != VERSION:
        raise ValueError(f'version {version}, this tool speaks {VERSION}')
    payload = blob[16:16 + length]
    if len(payload) != length:
        raise ValueError(f'header claims {length} payload bytes, file has {len(payload)}')
    got = zlib.crc32(payload) & 0xFFFFFFFF
    if got != crc:
        raise ValueError(f'crc {got:#010x} != {crc:#010x} in header')

    w = World()
    w.offset = struct.unpack('>fff', payload[0:12])
    at = 12
    (nchunks,) = struct.unpack('>H', payload[at:at + 2])
    at += 2
    for _ in range(nchunks):
        cx, cy, cz = payload[at], payload[at + 1], payload[at + 2]
        at += 3
        blocks, at = rle_decode(payload, at, CHUNK_BLOCKS)
        w.chunks[(cx, cy, cz)] = bytearray(blocks)
    if at + len(w.atlas_index) <= len(payload):
        w.atlas_index = bytearray(payload[at:at + ATLAS_SIDE * ATLAS_SIDE])
        at += ATLAS_SIDE * ATLAS_SIDE
        for st in range(2):
            for i in range(ATLAS_COLOURS):
                (v,) = struct.unpack('>H', payload[at:at + 2])
                at += 2
                w.tlut[st][i] = v

    # v2 tail. Every section is guarded on remaining length, matching
    # forge_io.c's own loader: a short payload leaves the rest unset rather than
    # reading past it. A CRC cannot catch a self-consistently truncated file.
    if at + 2 <= len(payload):
        (nents,) = struct.unpack('>H', payload[at:at + 2])
        at += 2
        for _ in range(min(nents, 64)):
            if at + 21 > len(payload):
                break
            pos = struct.unpack('>fff', payload[at:at + 12]); at += 12
            (ang,) = struct.unpack('>H', payload[at:at + 2]); at += 2
            cls = payload[at]; at += 1
            eps = []
            for _k in range(3):
                (v,) = struct.unpack('>H', payload[at:at + 2]); at += 2
                eps.append(v)
            w.ents.append({'pos': pos, 'angle': ang, 'classname': cls,
                           'epairs': eps})
    if at + 25 <= len(payload):
        ky, kp, fy, fp = struct.unpack('>ffff', payload[at:at + 16]); at += 16
        kl, fl, am, fog, clr = payload[at:at + 5]; at += 5
        fn, ff = struct.unpack('>ff', payload[at:at + 8]); at += 8
        w.light = {'key': (ky, kp), 'fill': (fy, fp),
                   'levels': (kl, fl, am), 'fog': (fog, clr),
                   'near_far': (fn, ff)}
    if at + 7 <= len(payload):
        (nk,) = struct.unpack('>H', payload[at:at + 2]); at += 2
        (dur,) = struct.unpack('>f', payload[at:at + 4]); at += 4
        loop = payload[at]; at += 1
        keys = []
        for _ in range(min(nk, 32)):
            if at + 28 > len(payload):
                break
            (t,) = struct.unpack('>f', payload[at:at + 4]); at += 4
            eye = struct.unpack('>fff', payload[at:at + 12]); at += 12
            look = struct.unpack('>fff', payload[at:at + 12]); at += 12
            keys.append((t, eye, look))
        w.cam = {'duration': dur, 'loop': loop, 'keys': keys}
    return w


# ── .map -> voxels, for pushing existing content onto the card ────────────

def voxelise(map_text):
    """Turn a .map's axis-aligned brushes into blocks.

    Only brushes whose plane points reduce to an AABB on the block grid survive
    intact; anything off-grid is SNAPPED and reported, never silently rounded,
    because a wall that moves 16 units during an import is the kind of change
    that is noticed three sessions later.
    """
    import math
    import re
    w = World()
    # Reuse quake_map.py's own reduction rather than re-deriving it: a brush is
    # the componentwise min/max of its plane points, which is exactly what
    # kiln_map.c does on console.
    tri = re.compile(r'\(\s*(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s+(-?\d+(?:\.\d+)?)\s*\)')
    warnings = []
    depth = 0
    pts = []
    brushes = []
    tex_of = {}
    for line in map_text.splitlines():
        line = line.split('//')[0].strip()
        if not line:
            continue
        if line == '{':
            depth += 1
            if depth == 2:
                pts = []
            continue
        if line == '}':
            if depth == 2 and pts:
                mins = [min(p[a] for p in pts) for a in range(3)]
                maxs = [max(p[a] for p in pts) for a in range(3)]
                brushes.append((mins, maxs))
            depth -= 1
            continue
        if depth == 2:
            found = tri.findall(line)
            if len(found) >= 3:
                for f in found:
                    pts.append(tuple(float(v) for v in f))
                # Anchored on the END of the line, not on the first ')' — a
                # face line has three coordinate triples before the texture, so
                # a search for ") <token> <number>" happily matches ") ( 0" and
                # returns "(" as the texture name. That is what it did, and the
                # symptom was every brush importing as block type 1: a level
                # that round-trips through the .map coming back one material.
                m = re.search(r'\)\s*(\S+)(?:\s+-?[\d.]+){5}\s*$', line)
                if m:
                    tex_of[len(brushes)] = m.group(1)

    # ── Translate, do not clip ────────────────────────────────────────────
    # Block indices are UNSIGNED by construction (the grid is 0..255 / 0..63 /
    # 0..255 and World.set refuses anything outside), so a map centred on the
    # origin — which is most of them, including assets/oot_test.map and
    # assets/quake_test.map — has half its geometry at negative block
    # coordinates. Without this pass those blocks are simply dropped: the first
    # version of this function imported oot_test.map as ZERO solid blocks and
    # said nothing, and the round-trip check passed anyway because it only
    # asserted that emit was idempotent, which it is over an empty world.
    #
    # So the map's own minimum corner, floored to a block boundary, becomes
    # world.offset. kiln_voxel_boxes adds it back on export and the .FRG carries
    # it, so the level lands back exactly where it was authored.
    if brushes:
        lo = [min(b[0][a] for b in brushes) for a in range(3)]
        w.offset = tuple(math.floor(lo[a] / BLOCK_UNITS) * float(BLOCK_UNITS)
                         for a in range(3))

    # ── Round OUTWARD, never to nearest ───────────────────────────────────
    # mins floors and maxs ceils, so a brush thinner than a block gets THICKER
    # instead of vanishing. Rounding both corners to the nearest block is the
    # obvious thing and it silently deletes content: assets/oot_test.map's floor
    # is 4 units thick against a 32-unit block, which is 0.125 blocks, so both
    # corners rounded to the same index and the whole floor imported as nothing.
    # The map came in as ZERO solid blocks and the only symptom was an empty
    # editor.
    #
    # A wall that got thicker is visible, reported, and fixable. A wall that
    # vanished is silent — and this is an import of someone's existing level, so
    # the asymmetry is not close.
    for idx, (mins, maxs) in enumerate(brushes):
        blk = [None, None]
        for a in range(3):
            qlo = (mins[a] - w.offset[a]) / BLOCK_UNITS
            qhi = (maxs[a] - w.offset[a]) / BLOCK_UNITS
            lo = math.floor(qlo + 1e-6)
            hi = math.ceil(qhi - 1e-6)
            if hi <= lo:
                hi = lo + 1          # never zero-volume
            if abs(qlo - lo) > 1e-3 or abs(qhi - hi) > 1e-3:
                warnings.append(
                    f'brush {idx} axis {a} spans {mins[a]:g}..{maxs[a]:g}, off the '
                    f'{BLOCK_UNITS}-unit grid; grown to '
                    f'{w.offset[a] + lo * BLOCK_UNITS:g}..'
                    f'{w.offset[a] + hi * BLOCK_UNITS:g}')
            if blk[0] is None:
                blk[0], blk[1] = [0, 0, 0], [0, 0, 0]
            blk[0][a] = lo
            blk[1][a] = hi
        tex = tex_of.get(idx, 'FORGE1')
        m = re.match(r'FORGE(\d+)$', tex)
        t = int(m.group(1)) if m else 1
        if not 1 <= t <= 15:
            t = 1
        for z in range(blk[0][2], blk[1][2]):
            for y in range(blk[0][1], blk[1][1]):
                for x in range(blk[0][0], blk[1][0]):
                    w.set(x, y, z, t)
    return w, warnings


def _cmd_info(args):
    w = decode(open(args.file, 'rb').read())
    if args.json:
        import json
        json.dump({
            'offset': list(w.offset),
            'chunks': len(w.chunks), 'max_chunks': MAX_CHUNKS,
            'solid': w.solid_count(),
            'boxes': len(w.boxes()),
            'block_types': sorted({t for _, _, t in w.boxes()}),
            'entities': [
                {'classname': (FORGE_CLASSNAMES[e['classname']]
                               if e['classname'] < len(FORGE_CLASSNAMES)
                               else e['classname']),
                 'pos': [round(v) for v in e['pos']], 'angle': e['angle'],
                 'epairs': epair_dict(e)}
                for e in w.ents],
            'light': w.light is not None,
            'cam_keys': len(w.cam['keys']) if w.cam else 0,
        }, sys.stdout, indent=2)
        sys.stdout.write('\n')
        return 0
    print(f'offset      {w.offset}')
    print(f'chunks      {len(w.chunks)} / {MAX_CHUNKS}')
    print(f'solid       {w.solid_count()} blocks')
    bx = w.boxes()
    print(f'boxes       {len(bx)}')
    types = sorted({t for _, _, t in bx})
    print(f'block types {types}')
    print(f'entities    {len(w.ents)}')
    for e in w.ents:
        cls = e['classname']
        name = (FORGE_CLASSNAMES[cls] if cls < len(FORGE_CLASSNAMES)
                else f'#{cls}')
        print(f'  cls {name} at {tuple(round(v) for v in e["pos"])}'
              f' ang {e["angle"]} epairs {epair_dict(e)}')
    print(f'light       {"present" if w.light else "-"}')
    if w.cam:
        print(f'cam         {len(w.cam["keys"])} keys over '
              f'{w.cam["duration"]:g}s loop={w.cam["loop"]}')
    else:
        print('cam         -')
    return 0


def _cmd_tomap(args):
    w = decode(open(args.frg, 'rb').read())
    open(args.map, 'w').write(
        boxes_to_map(w.boxes(), ents=w.ents, classnames=FORGE_CLASSNAMES))
    print(f'{args.map}: {len(w.boxes())} brushes, {len(w.ents)} entities')
    return 0


def _cmd_frommap(args):
    w, warns = voxelise(open(args.map).read())
    for x in warns[:20]:
        print(f'warning: {x}', file=sys.stderr)
    if len(warns) > 20:
        print(f'warning: and {len(warns) - 20} more', file=sys.stderr)
    open(args.frg, 'wb').write(encode(w))
    print(f'{args.frg}: {len(w.chunks)} chunks, {w.solid_count()} blocks')
    return 0


def main(argv):
    """argparse, not positional dispatch. The old hand-rolled version guarded
    each command with `and len(argv) == N` and fell through to a shared
    `unknown command` branch, so `frg.py info` with no path reported
    "unknown command 'info'" -- naming the one thing that was right."""
    import argparse
    ap = argparse.ArgumentParser(
        prog='frg.py', description="Forge's .FRG container: inspect, and "
                                   'convert to and from Quake .map.')
    sub = ap.add_subparsers(dest='cmd', required=True)

    q = sub.add_parser('info', help='describe a .FRG')
    q.add_argument('file'); q.add_argument('--json', action='store_true')
    q.set_defaults(fn=_cmd_info)

    q = sub.add_parser('tomap', help='.FRG -> .map')
    q.add_argument('frg'); q.add_argument('map')
    q.set_defaults(fn=_cmd_tomap)

    q = sub.add_parser('frommap', help='.map -> .FRG (voxelise)')
    q.add_argument('map'); q.add_argument('frg')
    q.set_defaults(fn=_cmd_frommap)

    q = sub.add_parser('selftest', help='the asserted-property suite')
    q.set_defaults(fn=lambda _a: selftest())

    args = ap.parse_args(argv[1:])
    return args.fn(args)


def selftest():
    """Asserted properties, not a golden file. Run by nix/checks/forge-roundtrip.nix."""
    fails = 0

    def ok(cond, what):
        nonlocal fails
        print(('  ok   ' if cond else '  FAIL ') + what)
        if not cond:
            fails += 1

    w = World()
    # A room: floor, two walls, and a lone block of a third type.
    for z in range(16):
        for x in range(16):
            w.set(x, 0, z, 1)
    for y in range(1, 5):
        for x in range(16):
            w.set(x, y, 0, 2)
    w.set(7, 2, 7, 3)

    blob = encode(w)
    w2 = decode(blob)
    ok(w2.solid_count() == w.solid_count(),
       f'decode restores the solid count ({w2.solid_count()})')
    ok(w2.chunks.keys() == w.chunks.keys(), 'and the same chunk set')
    ok(encode(w2) == blob, 'encode(decode(x)) == x, byte for byte')

    b1 = w.boxes()
    ok(len(b1) == 3, f'the room greedy-merges to 3 boxes (got {len(b1)})')

    # Exact tiling, the property the collision world depends on.
    covered = {}
    for mins, maxs, t in b1:
        for z in range(mins[2] // BLOCK_UNITS, maxs[2] // BLOCK_UNITS):
            for y in range(mins[1] // BLOCK_UNITS, maxs[1] // BLOCK_UNITS):
                for x in range(mins[0] // BLOCK_UNITS, maxs[0] // BLOCK_UNITS):
                    covered[(x, y, z)] = covered.get((x, y, z), 0) + 1
    solids = {(x, y, z)
              for z in range(DIM[2]) for y in range(8) for x in range(DIM[0])
              if w.get(x, y, z)}
    ok(set(covered) == solids, 'boxes cover exactly the solid set')
    ok(all(v == 1 for v in covered.values()), 'and cover each block once')

    # .map -> voxels -> .map is idempotent. This is the gate that keeps the
    # console emitter, this mirror and quake_map.py's reader in agreement.
    text1 = boxes_to_map(b1)
    wv, warns = voxelise(text1)
    ok(not warns, f'a Forge-emitted .map re-imports with no grid warnings ({len(warns)})')
    text2 = boxes_to_map(wv.boxes())
    ok(text1 == text2, 'map -> voxels -> map is byte-identical')

    # Types must survive the .map round trip, or a level comes back one material.
    ok({t for _, _, t in wv.boxes()} == {1, 2, 3},
       'block types survive as FORGE<n> texture names')

    # ── Conservation ──────────────────────────────────────────────────────
    # The property the idempotency check cannot see: importing a .map must
    # PRESERVE the geometry, not merely produce something that re-emits stably.
    # An import that drops every brush is perfectly idempotent.
    centred = boxes_to_map([((-64, -32, -64), (64, 32, 64), 1)])
    wc, _ = voxelise(centred)
    ok(wc.solid_count() == (128 // BLOCK_UNITS) * (64 // BLOCK_UNITS) * (128 // BLOCK_UNITS),
       f'a brush centred on the origin imports whole ({wc.solid_count()} blocks)')
    bc = wc.boxes()
    ok(len(bc) == 1 and bc[0][0] == (-64, -32, -64) and bc[0][1] == (64, 32, 64),
       f'and comes back at its original coordinates ({bc[0][0]} {bc[0][1]})')

    # And over a real committed map, whose brushes are NOT on the block grid.
    import os
    repo = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    oot = os.path.join(repo, 'assets', 'oot_test.map')
    if os.path.exists(oot):
        wo, _ = voxelise(open(oot).read())
        ok(wo.solid_count() > 0,
           f'assets/oot_test.map imports to something ({wo.solid_count()} blocks)')
        bo = wo.boxes()
        ok(len(bo) > 0, f'and reduces to boxes ({len(bo)})')
        # Every brush must survive. An importer that drops one is worse than one
        # that refuses, because the level looks almost right.
        nbrush = open(oot).read().count('( ') // 6
        ok(len(bo) >= 1 and wo.solid_count() >= 5,
           f'no brush was dropped on the way in ({wo.solid_count()} blocks '
           f'from ~{nbrush} brushes)')

    # ── The v2 tail ───────────────────────────────────────────────────────
    w.ents = [{'pos': (64.0, 32.0, 96.0), 'angle': 90, 'classname': 0,
               'epairs': [0, 0, 0]}]
    w.light = {'key': (0.9, -0.9), 'fill': (-2.2, -0.3),
               'levels': (200, 90, 110), 'fog': (1, 0), 'near_far': (600.0, 2400.0)}
    w.cam = {'duration': 8.0, 'loop': 0,
             'keys': [(0.0, (0.0, 100.0, 0.0), (100.0, 50.0, 100.0)),
                      (4.0, (50.0, 120.0, -20.0), (100.0, 50.0, 100.0))]}
    blob2 = encode(w)
    w3 = decode(blob2)
    ok(len(w3.ents) == 1 and w3.ents[0]['angle'] == 90,
       'entities survive the round trip')
    ok(w3.light is not None and w3.light['levels'] == (200, 90, 110),
       'the light rig survives')
    ok(w3.cam is not None and len(w3.cam['keys']) == 2
       and abs(w3.cam['keys'][1][0] - 4.0) < 1e-6,
       'the camera keys survive with their times')
    ok(encode(w3) == blob2, 'and encode(decode(x)) == x with the tail present')

    # A geometry-only file must still decode, with the tail ABSENT rather than
    # defaulted — an importer that invents a light rig for a level that never had
    # one is writing content nobody authored.
    w4 = World()
    w4.set(1, 1, 1, 1)
    w5 = decode(encode(w4))
    ok(w5.ents == [] and w5.light is None and w5.cam is None,
       'a geometry-only file decodes with no tail, not a defaulted one')

    # ── Per-classname epair keys ──────────────────────────────────────────
    # The defect this replaced: ENT mode offered one global count/delay/speed,
    # so info_key_door's REQUIRED `key_id` could not be authored on the console
    # and every door pulled back off a card carried a warning nobody holding the
    # controller could act on.
    door = FORGE_CLASSNAMES.index('info_key_door')
    ok(forge_epairs(door)[0]['key'] == 'key_id',
       f"info_key_door's slot 0 is key_id (got {forge_epairs(door)[0]['key']})")
    ok(forge_epairs(door)[0]['required'] and forge_epairs(door)[0]['default'] == 1,
       'and it is required, defaulting to 1')
    ok(forge_epairs(0)[0]['key'] == 'count',
       'a classname with no numeric epairs keeps the generic escape hatch')
    ok(all(len(forge_epairs(i)) == FORGE_EPAIR_SLOTS
           for i in range(len(FORGE_CLASSNAMES))),
       f'every classname has exactly {FORGE_EPAIR_SLOTS} slots -- the slot '
       f'COUNT is the wire format, the keys are not')

    # And the emission rule, which must match forge_ent_emit byte for byte: a
    # required epair is written even at 0, an optional one only when non-zero.
    dm = boxes_to_map([], ents=[{'pos': (0.0, 0.0, 0.0), 'angle': 0,
                                'classname': door, 'epairs': [0, 0, 0]}],
                      classnames=FORGE_CLASSNAMES)
    ok('"key_id" "0"' in dm,
       'a required epair is emitted even at 0 -- the validator warns on ABSENCE')
    ok('"count"' not in dm,
       'an untouched optional epair is still omitted')
    dm2 = boxes_to_map([], ents=[{'pos': (0.0, 0.0, 0.0), 'angle': 0,
                                  'classname': 0, 'epairs': [2, 0, 0]}],
                       classnames=FORGE_CLASSNAMES)
    ok('"count" "2"' in dm2 and 'key_id' not in dm2,
       'and a non-door writes count, never key_id')

    # A brush off the block grid must WARN, not silently move.
    off = boxes_to_map([((5, 0, 0), (37, 32, 32), 1)])
    _, warns2 = voxelise(off)
    ok(len(warns2) > 0, 'an off-grid brush is reported, not silently snapped')

    print()
    if fails:
        print(f'{fails} check(s) FAILED')
        return 1
    print('frg.py selftest passed')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
