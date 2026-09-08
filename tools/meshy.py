#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""meshy.py — generate a model with Meshy AI and land it on this pipeline.

    tools/meshy.py balance
    tools/meshy.py gen --name shotgun --prompt "battered pump-action shotgun"
    tools/meshy.py import shotgun --material shotgun_body --veil-class world
    tools/meshy.py selftest

An **authoring-time tool**, in the same category as tools/blender-mcp: it runs
outside `nix build`, against content that is not committed yet, so a generation
can be iterated on. What it produces is then checked into `assets/` and wired
into flake.nix by hand, which puts it on the same hermetic,
twice-built-and-hash-compared path as every hand-authored model.

That boundary is not a preference, it is forced. This engine's builds have no
`fetchurl`, no fixed-output derivation and no `__noChroot` anywhere in them,
and a text-to-3D call returns a DIFFERENT mesh every time it is made. Either
property alone would rule out putting generation inside a derivation. So the
generated artifact is the source, and its regeneration discipline is policed
by a check in the consuming repo, the way a generated header or a rig JSON
already is.

── What the gap actually is ───────────────────────────────────────────────
Meshy emits a 30k-triangle mesh with a 2048x2048 truecolour PBR texture set.
This hardware wants a few hundred triangles and a 4bpp texture with SIXTEEN
colours. Every part of that gap fails SILENTLY if you skip it, which is why
this is a tool and not a curl invocation:

  * tools/veil_palette.py refuses truecolour input on purpose, because
    `format = "CI4"` makes mksprite quantise and veil_palette.py reads the
    PNG's own palette — two independent quantisations of one image, and the
    indices do not correspond. The runtime then binds a TLUT built from one
    palette over texels indexed against another. Nothing reports an error;
    the model is simply painted in the wrong colours.

    So this file owns the ONE quantisation. Its output is an indexed PNG of
    at most 16 exact colours, which is the input veil_palette.py accepts, and
    after that point both tools see the same palette by construction.

  * f3d_inject.py takes ONE `size` scalar and applies it to both S and T
    (there is no `sizeT`), and raises on a non-power-of-two. So the output is
    square and a power of two, asserted rather than hoped for.

  * A material with no name is SKIPPED by gltf_to_t3d, silently, with a zero
    exit status. Meshy names its materials things like `material_0`. Renaming
    is the consuming repo's job (it is the game that knows what the material
    is called) but this tool reports what it found so a rename cannot be
    forgotten.

  * Triangle count is deliberately NOT reduced here. Ask Meshy for headroom
    and decimate in Blender, where the ratio is a reviewable number in a
    table next to every other model's. Handing the final count to a remote
    decimator hands it silhouette decisions too.

── Why value separation is checked ────────────────────────────────────────
A palette swap collapses hue. Two palette entries that differ only in hue are
two regions that become indistinguishable the moment the filter rises — and
that is not visible in the source texture, only in the game, only sometimes.
`--min-value-gap` fails on it at authoring time instead. It is a real
constraint of the mechanic, not a style rule, and `--min-value-gap 0` turns it
off for a texture that genuinely wants it off.

── Determinism ────────────────────────────────────────────────────────────
Everything after the download is reproducible: box downscale, median cut with
dithering OFF, and a palette ordered by `sorted()` rather than by first
appearance (first-seen order depends on the scan, and would change if the
image were re-exported with a different row order). `selftest` asserts it by
running the pass twice and comparing bytes, because the build compares every
asset against a second build of itself and anything derived from this has to
survive that.
"""

import argparse
import hashlib
import json
import math
import os
import shutil
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import Counter
from datetime import date
from pathlib import Path

API = "https://api.meshy.ai"

# The four classes tools/veil_palette.py defines. Repeated here as a value
# check only — that file remains the authority on what each one means.
VEIL_CLASSES = ("world", "demon", "phantom", "eyes")

# veil_palette.py's WORLD_LO/WORLD_HI. A `world` material may not own true
# black or true white: those two ends are rationed to the subject of the shot,
# so a world texture that reaches them steals contrast from the thing the
# player is supposed to be looking at.
WORLD_LO, WORLD_HI = 0.45, 0.88

CI4_COLOURS = 16          # 4 bits per texel
DEFAULT_TEX_SIZE = 32
DEFAULT_POLYCOUNT = 2000  # headroom; the Blender step decimates to budget
MIN_VALUE_GAP = 0.015     # see the module docstring

POLL_INTERVAL = 5.0
POLL_TIMEOUT = 1800.0

# Meshy's per-endpoint paths. Text-to-3D is v2 (it has the preview/refine
# two-stage flow); everything else is v1.
PATHS = {
    "text-to-3d": "/openapi/v2/text-to-3d",
    "image-to-3d": "/openapi/v1/image-to-3d",
    "remesh": "/openapi/v1/remesh",
    "balance": "/openapi/v1/balance",
}


def die(msg):
    raise SystemExit("meshy: " + msg)


# ── credentials ────────────────────────────────────────────────────────────
def api_key(explicit=None, keyfile=None):
    """--api-key, then $MESHY_API_KEY, then a key file. Never logged.

    The file is last rather than first so that an environment variable can
    override a stale checkout without editing it, and it exists at all so an
    assistant driving this tool does not need the key pasted into a prompt
    every session. It must be gitignored in the repo that holds it.
    """
    if explicit:
        return explicit.strip()
    env = os.environ.get("MESHY_API_KEY")
    if env:
        return env.strip()
    candidates = [Path(keyfile)] if keyfile else [
        Path.cwd() / ".meshy_key",
        Path.home() / ".config" / "meshy" / "key",
    ]
    for path in candidates:
        if path.is_file():
            key = path.read_text().strip()
            if key:
                mode = path.stat().st_mode & 0o777
                if mode & 0o077:
                    print("meshy: warning: %s is mode %o; chmod 600 it"
                          % (path, mode), file=sys.stderr)
                return key
    die("no API key. Pass --api-key, export MESHY_API_KEY, or write one to\n"
        "  ./.meshy_key   (and make sure it is gitignored)\n"
        "New keys: https://www.meshy.ai/settings/api — the value is shown once,\n"
        "at the moment it is created, and starts with msy_.")


# ── HTTP ───────────────────────────────────────────────────────────────────
def _call(method, path, key, body=None, timeout=60.0):
    """One request. The key goes in a header and nowhere else.

    Not in a query string: a URL is the part of a request that gets logged by
    proxies, written into shell history and printed in error messages.
    """
    url = API + path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + key)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read()
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:600]
        # 402 and 429 are the two a caller can actually act on, so name them.
        hint = {
            401: "  the key was rejected. Check it at meshy.ai/settings/api.",
            402: "  out of credits. `meshy.py balance` shows the count.",
            429: "  rate limited (20 req/s, and a per-tier concurrent task\n"
                 "  queue). Retry, or wait for a running task to finish.",
        }.get(e.code, "")
        die("%s %s -> HTTP %d\n  %s%s"
            % (method, path, e.code, detail, ("\n" + hint) if hint else ""))
    except urllib.error.URLError as e:
        die("%s %s failed: %s" % (method, path, e.reason))
    return json.loads(raw) if raw else {}


def get(path, key):
    return _call("GET", path, key)


def post(path, key, body):
    return _call("POST", path, key, body)


def poll(kind, task_id, key, label="", quiet=False,
         interval=POLL_INTERVAL, timeout=POLL_TIMEOUT):
    """Block until a task leaves PENDING/IN_PROGRESS. Returns the task object.

    Polling rather than the /stream SSE endpoint: this is a batch tool run from
    a shell, and a plain GET loop has no partial-read or reconnect semantics to
    get wrong.
    """
    base = PATHS[kind]
    start = time.time()
    last = None
    while True:
        task = get("%s/%s" % (base, task_id), key)
        status = task.get("status")
        progress = task.get("progress", 0)
        line = "%s %s %3d%%" % (status, label, progress)
        if not quiet and line != last:
            print("  %-40s" % line, flush=True)
            last = line
        if status == "SUCCEEDED":
            return task
        if status in ("FAILED", "CANCELED"):
            err = (task.get("task_error") or {}).get("message") or status
            die("task %s %s: %s" % (task_id, status.lower(), err))
        if time.time() - start > timeout:
            die("task %s still %s after %.0f s. It is not lost — "
                "`meshy.py status --id %s` and then `meshy.py fetch`."
                % (task_id, status, timeout, task_id))
        time.sleep(interval)


def download(url, dest):
    """Fetch a presigned asset URL to a file. Returns bytes written."""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=300) as r, \
                open(dest, "wb") as f:
            shutil.copyfileobj(r, f)
    except (urllib.error.URLError, urllib.error.HTTPError) as e:
        die("downloading %s failed: %s\n  These URLs are presigned and expire "
            "(the task carries expires_at, roughly three days). "
            "`meshy.py fetch --id <id>` re-reads them from the task."
            % (dest.name, e))
    return dest.stat().st_size


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ── colour ─────────────────────────────────────────────────────────────────
def _pixels(im):
    """The image's pixels as a flat sequence, across Pillow versions.

    Pillow 12 deprecates getdata() in favour of get_flattened_data() and
    removes it in 14. Both return the same shape — tuples for RGB, ints for
    mode P — so this is a name change, not a behaviour change, and pinning to
    one of them would make this file's warning output depend on which Pillow
    the caller happens to have.
    """
    fn = getattr(im, "get_flattened_data", None)
    return list(fn() if fn is not None else im.getdata())


def luma(rgb):
    """Rec.709 luminance of an sRGB triple, 0..1.

    Computed on the sRGB values rather than on linearised ones ON PURPOSE:
    the question being asked is "would a viewer tell these two apart once hue
    is gone", and sRGB is already roughly perceptual. Linearising first would
    crush the dark end and let two near-blacks pass the separation check that
    a player cannot distinguish.
    """
    r, g, b = (c / 255.0 for c in rgb[:3])
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def value_ladder(colours):
    """[(luma, rgb)] sorted by luminance, plus the smallest adjacent gap."""
    ladder = sorted((luma(c), tuple(c)) for c in colours)
    gaps = [ladder[i + 1][0] - ladder[i][0] for i in range(len(ladder) - 1)]
    return ladder, (min(gaps) if gaps else 1.0)


def merge_by_value(pixels, min_gap):
    """Collapse colour pairs closer in value than `min_gap`. Deterministic.

    Repeatedly merges the closest adjacent pair in the value ladder, keeping
    whichever of the two covers more texels (ties broken by the RGB tuple, so
    the result does not depend on dict order). Each merge costs one palette
    entry and buys separation — which is the right trade for CI4, where the
    budget is 16 and an entry nobody can see is an entry wasted.
    """
    counts = Counter(pixels)
    remap = {}
    while len(counts) > 1:
        ladder, gap = value_ladder(counts)
        if gap >= min_gap:
            break
        i = min(range(len(ladder) - 1),
                key=lambda k: (ladder[k + 1][0] - ladder[k][0],
                               ladder[k][1], ladder[k + 1][1]))
        a, b = ladder[i][1], ladder[i + 1][1]
        keep, drop = ((a, b) if (counts[a], tuple(reversed(a)))
                      >= (counts[b], tuple(reversed(b))) else (b, a))
        counts[keep] += counts[drop]
        del counts[drop]
        # Anything already pointed at `drop` follows it.
        for src, dst in list(remap.items()):
            if dst == drop:
                remap[src] = keep
        remap[drop] = keep
    if not remap:
        return pixels, 0
    return [remap.get(p, p) for p in pixels], len(remap)


def spread_into(colours, lo, hi):
    """Remap luminance linearly into [lo, hi], keeping hue. Deterministic.

    For veilClass = "world". The transform scales each RGB triple toward its
    target luminance rather than blending toward grey, so the hue survives —
    which matters even though the veil discards hue, because the veil is not
    always up and the cold palette is what the player sees most of the time.
    """
    ladder, _ = value_ladder(colours)
    if len(ladder) < 2:
        return {c: c for _, c in ladder}
    src_lo, src_hi = ladder[0][0], ladder[-1][0]
    span = src_hi - src_lo
    out = {}
    for value, rgb in ladder:
        t = 0.5 if span < 1e-9 else (value - src_lo) / span
        target = lo + t * (hi - lo)
        if value < 1e-6:
            # Pure black cannot be scaled toward a luminance. A neutral of the
            # target value is the only answer that preserves "this entry is
            # the darkest one" without inventing a hue.
            level = int(round(target * 255.0))
            out[rgb] = (level, level, level)
            continue
        k = target / value
        scaled = tuple(min(255, max(0, int(round(c * k)))) for c in rgb)
        out[rgb] = scaled
    return out


def to_indexed(im):
    """Truecolour -> mode P with an EXACT palette. Deterministic.

    The canonical implementation of the property everything downstream leans
    on: the PNG's palette IS the texture's palette, entry for entry, so
    mksprite and veil_palette.py cannot disagree about what index 7 means.

    PetaByte-Madness/tools/extract_demon_textures.py carries the same
    algorithm for the textures it lifts out of .glb files, and that repo's
    nix/checks/pm-meshy.nix asserts the two agree by running an
    already-conforming texture through this one and requiring no change.

    Note that a sorted palette is a CONVENTION, not a correctness property.
    Two orderings exist in the tree and both render correctly, because the
    palette and the indices always change together: extract_demon_textures.py
    sorts (so imp_hide.png and friends round-trip through here byte for
    byte), while machine_centaur_gen.py emits mc_face/mc_plate/mc_gore in
    authoring order (so they round-trip to the same picture and different
    bytes). Sorted is what this tool emits, for the reason above — it is the
    only ordering that does not depend on how the source was scanned.
    """
    from PIL import Image
    im = im.convert("RGB")
    px = _pixels(im)
    # sorted(), not first-seen order: first-seen depends on the scan and would
    # change if the image were ever re-exported with a different row order.
    colours = sorted(set(px))
    if len(colours) > CI4_COLOURS:
        die("%d distinct colours, CI4 allows %d. Quantise first — that is what "
            "the `ci4` subcommand is for." % (len(colours), CI4_COLOURS))
    lut = {c: i for i, c in enumerate(colours)}
    out = Image.new("P", im.size)
    out.putdata([lut[p] for p in px])
    flat = []
    for c in colours:
        flat += list(c)
    flat += [0, 0, 0] * (256 - len(colours))
    out.putpalette(flat)
    return out, colours


def make_ci4(im, size=DEFAULT_TEX_SIZE, veil_class="world",
             min_gap=MIN_VALUE_GAP, merge=False, quiet=False):
    """Any image -> a CI4-ready indexed PNG. The whole texture pass.

    Returns (indexed image, report dict). Raises rather than returning
    something almost right: every failure here is one that would otherwise
    reach the ROM as a texture that looks slightly wrong.
    """
    from PIL import Image

    if size < 1 or (size & (size - 1)):
        die("--tex-size %d is not a power of two. f3d_inject computes the RDP "
            "wrap mask as (size-1).bit_length() and rejects anything else." % size)
    if veil_class not in VEIL_CLASSES:
        die("--veil-class %r is not one of %s (tools/veil_palette.py defines "
            "them)" % (veil_class, ", ".join(VEIL_CLASSES)))

    report = {"source_size": list(im.size), "size": size,
              "veil_class": veil_class}
    im = im.convert("RGB")

    if im.size != (size, size):
        if im.size[0] != im.size[1] and not quiet:
            # Worth saying out loud: this changes the UV aspect. It is still
            # the right move, because `size` is one scalar for S and T.
            print("  note: source is %dx%d, not square — squaring it to %dx%d "
                  "changes the UV aspect. f3d_inject has one `size` for both "
                  "axes, so a non-square texture gets a wrong T mask."
                  % (im.size[0], im.size[1], size, size))
        # BOX, not the default: a box average over the source block is what
        # "shrink a photograph to a texture" means, it introduces no ringing,
        # and it is deterministic.
        im = im.resize((size, size), Image.Resampling.BOX)

    px = _pixels(im)
    report["distinct_in"] = len(set(px))

    if len(set(px)) > CI4_COLOURS:
        # Median cut with dithering OFF. Dithering is not a style choice at
        # this scale: it scatters isolated indices across a 32x32 texture,
        # which reads as noise on console AND destroys the "few flat colours"
        # property the palette swap depends on.
        q = im.quantize(colors=CI4_COLOURS,
                        method=Image.Quantize.MEDIANCUT,
                        dither=Image.Dither.NONE)
        im = q.convert("RGB")
        px = _pixels(im)

    if merge:
        px, merged = merge_by_value(px, min_gap)
        report["merged"] = merged
        if merged and not quiet:
            print("  merged %d colour(s) closer than %.3f in value" %
                  (merged, min_gap))

    if veil_class == "world":
        mapping = spread_into(set(px), WORLD_LO, WORLD_HI)
        px = [mapping[p] for p in px]
        if not quiet:
            print("  world class: luminance spread into [%.2f, %.2f] — a world "
                  "material does not own true black or true white"
                  % (WORLD_LO, WORLD_HI))

    im = Image.new("RGB", (size, size))
    im.putdata(px)
    out, colours = to_indexed(im)

    ladder, gap = value_ladder(colours)
    report["colours"] = len(colours)
    report["min_value_gap"] = round(gap, 5)
    report["value_ladder"] = [round(v, 4) for v, _ in ladder]

    if not quiet:
        print("  %d colours, value ladder %s"
              % (len(colours), " ".join("%.2f" % v for v, _ in ladder)))
    if min_gap > 0 and gap < min_gap:
        near = [rgb for _, rgb in ladder]
        die("two palette entries are %.4f apart in value (floor %.4f).\n"
            "  Under a palette swap hue carries no information, so those two "
            "regions\n  become the same colour the moment the veil rises — and "
            "nothing at\n  runtime reports it.\n"
            "  Fix with --merge-by-value (costs a palette entry, buys "
            "separation),\n  or accept it with --min-value-gap 0.\n"
            "  ladder: %s"
            % (gap, min_gap, " ".join("%.3f" % v for v, _ in ladder)))
    assert out.mode == "P", out.mode
    assert out.size == (size, size), out.size
    return out, report


def save_png(im, dest):
    """Write with optimize=False so the bytes are a function of the pixels.

    optimize=True lets zlib pick a strategy, which is stable in practice but
    is not a documented property, and the consuming build compares this file
    against a second copy of itself.
    """
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    im.save(dest, "PNG", optimize=False)
    return dest


# ── subcommands ────────────────────────────────────────────────────────────
def cmd_balance(a):
    key = api_key(a.api_key, a.key_file)
    bal = get(PATHS["balance"], key)["balance"]
    print("%d credits" % bal)
    return 0


def cmd_status(a):
    key = api_key(a.api_key, a.key_file)
    base = PATHS[a.kind]
    if a.id:
        task = get("%s/%s" % (base, a.id), key)
        print(json.dumps(task, indent=2, sort_keys=True))
        return 0
    tasks = get("%s?page_size=%d&sort_by=-created_at" % (base, a.limit), key)
    if isinstance(tasks, dict):
        tasks = tasks.get("result", tasks.get("data", []))
    if not tasks:
        print("no %s tasks on this account" % a.kind)
        return 0
    for t in tasks:
        print("%-38s %-12s %3d%%  %-22s %s"
              % (t.get("id"), t.get("status"), t.get("progress", 0),
                 t.get("type", ""), (t.get("prompt") or "")[:44]))
    return 0


def _save_task_assets(task, outdir, quiet=False):
    """glb + base-colour texture out of a finished task. Returns a dict."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    urls = task.get("model_urls") or {}
    got = {}

    glb = urls.get("glb")
    if not glb:
        die("task %s has no glb in model_urls (has %s). Pass "
            "--formats glb next time." % (task.get("id"), sorted(urls)))
    n = download(glb, outdir / "model.glb")
    got["glb"] = {"path": str(outdir / "model.glb"), "bytes": n, "url": glb}
    if not quiet:
        print("  -> %s  (%.1f KB)" % (outdir / "model.glb", n / 1024.0))

    tex = task.get("texture_urls") or []
    base = (tex[0] or {}).get("base_color") if tex else None
    if base:
        n = download(base, outdir / "albedo.png")
        got["albedo"] = {"path": str(outdir / "albedo.png"), "bytes": n,
                         "url": base}
        if not quiet:
            print("  -> %s  (%.1f KB)" % (outdir / "albedo.png", n / 1024.0))
        # The other PBR maps are downloaded for provenance and never used:
        # F3D has no metallic/roughness/normal path at all.
        for extra in ("metallic", "roughness", "normal", "emission"):
            u = (tex[0] or {}).get(extra)
            if u:
                download(u, outdir / ("%s.png" % extra))
    elif not quiet:
        print("  note: no texture in this task (untextured preview?)")

    (outdir / "task.json").write_text(
        json.dumps(task, indent=2, sort_keys=True) + "\n")
    return got


def cmd_gen(a):
    key = api_key(a.api_key, a.key_file)
    outdir = Path(a.out_dir or ("build/meshy/%s" % a.name))

    before = get(PATHS["balance"], key)["balance"]
    print("── %s: %d credits before ──" % (a.name, before))

    preview_body = {
        "mode": "preview",
        "prompt": a.prompt,
        "ai_model": a.ai_model,
        "topology": a.topology,
        "target_polycount": a.polycount,
        "should_remesh": True,
        "target_formats": a.formats,
    }
    if a.pose:
        preview_body["pose_mode"] = a.pose
    if a.model_type:
        preview_body["model_type"] = a.model_type

    print("preview: %r" % a.prompt)
    preview_id = post(PATHS["text-to-3d"], key, preview_body)["result"]
    print("  task %s" % preview_id)
    preview = poll("text-to-3d", preview_id, key, label="preview")

    task = preview
    refine_id = None
    if not a.no_refine:
        refine_body = {"mode": "refine", "preview_task_id": preview_id,
                       "texture_resolution": a.texture_resolution,
                       "enable_pbr": a.pbr,
                       "target_formats": a.formats}
        if a.texture_prompt:
            refine_body["texture_prompt"] = a.texture_prompt
        print("refine:  %s" % (a.texture_prompt or "(from the preview prompt)"))
        refine_id = post(PATHS["text-to-3d"], key, refine_body)["result"]
        print("  task %s" % refine_id)
        task = poll("text-to-3d", refine_id, key, label="refine")

    got = _save_task_assets(task, outdir)
    after = get(PATHS["balance"], key)["balance"]
    print("── spent %d credits, %d left ──" % (before - after, after))

    meta = {
        "name": a.name,
        "source": "text-to-3d",
        "generated": date.today().isoformat(),
        "prompt": a.prompt,
        "texture_prompt": a.texture_prompt,
        "params": {k: v for k, v in preview_body.items()
                   if k not in ("mode", "prompt")},
        "tasks": {"preview": preview_id, "refine": refine_id},
        "consumed_credits": before - after,
        "materials_found": [m.get("name") for m in
                            _glb_materials(outdir / "model.glb")],
        "downloads": got,
    }
    (outdir / "meshy.json").write_text(
        json.dumps(meta, indent=2, sort_keys=True) + "\n")
    print("\nmaterials in the .glb: %s" % (meta["materials_found"] or "none"))
    print("next:\n  %s import %s --material <name> --veil-class <class>"
          % (sys.argv[0], a.name))
    return 0


def cmd_image(a):
    key = api_key(a.api_key, a.key_file)
    outdir = Path(a.out_dir or ("build/meshy/%s" % a.name))

    src = a.image
    if not src.startswith(("http://", "https://", "data:")):
        # A local file becomes a data URI: the endpoint accepts one, and it
        # avoids needing anywhere public to put the reference art.
        import base64
        import mimetypes
        p = Path(src)
        if not p.is_file():
            die("no such image: %s" % src)
        mime = mimetypes.guess_type(p.name)[0] or "image/png"
        src = "data:%s;base64,%s" % (
            mime, base64.b64encode(p.read_bytes()).decode())

    before = get(PATHS["balance"], key)["balance"]
    print("── %s: %d credits before ──" % (a.name, before))

    body = {"image_url": src, "ai_model": a.ai_model,
            "topology": a.topology, "target_polycount": a.polycount,
            "should_remesh": True, "should_texture": True,
            "enable_pbr": a.pbr, "target_formats": a.formats}
    if a.pose:
        body["pose_mode"] = a.pose
    if a.texture_prompt:
        body["texture_prompt"] = a.texture_prompt

    task_id = post(PATHS["image-to-3d"], key, body)["result"]
    print("  task %s  (image-to-3d is single stage: no preview/refine)"
          % task_id)
    task = poll("image-to-3d", task_id, key, label="image-to-3d")

    got = _save_task_assets(task, outdir)
    after = get(PATHS["balance"], key)["balance"]
    print("── spent %d credits, %d left ──" % (before - after, after))

    meta = {
        "name": a.name,
        "source": "image-to-3d",
        "generated": date.today().isoformat(),
        "image": a.image if src is a.image else ("(inlined) " + a.image),
        "texture_prompt": a.texture_prompt,
        "params": {k: v for k, v in body.items() if k != "image_url"},
        "tasks": {"image": task_id},
        "consumed_credits": before - after,
        "materials_found": [m.get("name") for m in
                            _glb_materials(outdir / "model.glb")],
        "downloads": got,
    }
    (outdir / "meshy.json").write_text(
        json.dumps(meta, indent=2, sort_keys=True) + "\n")
    print("\nmaterials in the .glb: %s" % (meta["materials_found"] or "none"))
    return 0


def cmd_fetch(a):
    key = api_key(a.api_key, a.key_file)
    task = get("%s/%s" % (PATHS[a.kind], a.id), key)
    if task.get("status") != "SUCCEEDED":
        die("task %s is %s, nothing to fetch" % (a.id, task.get("status")))
    outdir = Path(a.out_dir or ("build/meshy/%s" % (a.name or a.id)))
    _save_task_assets(task, outdir)
    return 0


def cmd_ci4(a):
    """Quantise one image. Useful with no Meshy involvement at all."""
    from PIL import Image
    im = Image.open(a.src)
    print("── %s ──" % a.src)
    out, report = make_ci4(im, size=a.tex_size, veil_class=a.veil_class,
                           min_gap=a.min_value_gap, merge=a.merge_by_value)
    dest = save_png(out, a.out or a.src)
    print("  -> %s  (%dx%d, %d colours, mode P)"
          % (dest, out.size[0], out.size[1], report["colours"]))
    return 0


# ── the .glb reader (just enough to name materials) ────────────────────────
def _glb_materials(path):
    """The `materials` array out of a .glb's JSON chunk, or [] if unreadable.

    Deliberately tiny: this only ever needs the material names, so that
    `gen` can print them and `import` can refuse a rename that does not match
    anything. The real geometry reader is the consuming repo's Blender step.
    """
    import struct
    path = Path(path)
    if not path.is_file():
        return []
    data = path.read_bytes()
    if data[:4] != b"glTF":
        return []
    off = 12
    while off + 8 <= len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        if ctype == 0x4E4F534A:  # JSON
            return json.loads(data[off + 8:off + 8 + clen]).get("materials", [])
        off += 8 + clen
    return []


def cmd_import(a):
    """build/meshy/<name>/ -> tracked assets, plus a provenance manifest.

    The only stage that writes into `assets/`, and the only one a build ever
    sees the output of.
    """
    from PIL import Image

    work = Path(a.work_dir or ("build/meshy/%s" % a.name))
    assets = Path(a.assets)
    glb = work / "model.glb"
    albedo = work / "albedo.png"
    if not glb.is_file():
        die("no %s. Run `gen` first, or `fetch --id <id> --name %s`."
            % (glb, a.name))
    if not albedo.is_file():
        die("no %s — the task had no base-colour texture. An untextured model "
            "cannot carry a veil palette; re-run without --no-refine." % albedo)

    names = [m.get("name") for m in _glb_materials(glb)]
    print("── %s ──" % a.name)
    print("  materials in the .glb: %s" % (names or "none"))
    if not names:
        die("%s has no materials. gltf_to_t3d SKIPS any primitive whose "
            "material is missing or unnamed — silently, with a zero exit "
            "status — so this would convert to a nearly empty .t3dm." % glb)
    if a.material in names:
        print("  '%s' is already the .glb's material name" % a.material)
    else:
        print("  the Blender step must rename %r -> %r; the material name is "
              "the contract\n     the f3d_inject spec, the palette table and "
              "the runtime all match on."
              % (names[0], a.material))

    # ── the texture ──
    print("  texture:")
    tex, report = make_ci4(Image.open(albedo), size=a.tex_size,
                           veil_class=a.veil_class, min_gap=a.min_value_gap,
                           merge=a.merge_by_value)

    dst_glb = assets / "models" / ("%s.glb" % a.name)
    dst_tex = assets / "textures" / ("%s.png" % a.material)
    dst_meta = assets / "meshy" / ("%s.json" % a.name)

    for dst in (dst_glb, dst_tex):
        if dst.exists() and not a.force:
            die("%s already exists. --force to replace it — and read the "
                "consuming repo's manifest check first, because replacing a "
                "committed asset is exactly the silent drift that check "
                "exists to catch." % dst)

    dst_glb.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(glb, dst_glb)
    save_png(tex, dst_tex)
    print("  -> %s  (%.1f KB, undecimated: the Blender step trims it)"
          % (dst_glb, dst_glb.stat().st_size / 1024.0))
    print("  -> %s  (%dx%d, %d colours)"
          % (dst_tex, a.tex_size, a.tex_size, report["colours"]))

    # ── provenance ──
    gen = {}
    if (work / "meshy.json").is_file():
        gen = json.loads((work / "meshy.json").read_text())
    manifest = {
        "name": a.name,
        "tool": "kiln tools/meshy.py",
        "imported": date.today().isoformat(),
        "generated": gen.get("generated"),
        "source": gen.get("source"),
        "prompt": gen.get("prompt"),
        "texture_prompt": gen.get("texture_prompt"),
        "params": gen.get("params", {}),
        "tasks": gen.get("tasks", {}),
        "consumed_credits": gen.get("consumed_credits"),
        "material": a.material,
        "material_in_glb": names[0] if names else None,
        "tex_size": a.tex_size,
        "veil_class": a.veil_class,
        "texture": report,
        # Presigned and short-lived — the task carries expires_at, roughly
        # three days. Kept as provenance (which model this came from), not as
        # something re-fetchable.
        "urls": {k: v.get("url") for k, v in
                 sorted((gen.get("downloads") or {}).items())},
        "urls_expire": True,
        # Keyed relative to the ASSETS directory, not to a repo root. The
        # check that verifies these receives assets/ as a Nix store path,
        # where a repo-relative key has nothing to resolve against.
        "sha256": {
            "models/%s.glb" % a.name: sha256(dst_glb),
            "textures/%s.png" % a.material: sha256(dst_tex),
        },
    }
    dst_meta.parent.mkdir(parents=True, exist_ok=True)
    dst_meta.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print("  -> %s  (provenance + hashes)" % dst_meta)

    # ── park the raw drop ──
    if a.archive:
        arc = Path(a.archive) / a.name
        arc.mkdir(parents=True, exist_ok=True)
        for f in sorted(work.iterdir()):
            if f.is_file():
                shutil.copyfile(f, arc / f.name)
        print("  -> %s/  (the raw drop: 2K albedo, unused PBR maps, task JSON)"
              % arc)

    print("\nstill to do by hand:\n"
          "  1. the model table entry (name, scale in METRES, decimate ratio,\n"
          "     material %r) and its size bound in the table's test\n"
          "  2. flake.nix: mkVeilTexture { name = %r; veilClass = %r; }, the\n"
          "     spec %s=tex0_decal,tex=textures/%s.png,size=%d,prim=1:1:1:1\n"
          "     — prim is load-bearing, RGB_MUL has no ONE operand — and the\n"
          "     model-%s output\n"
          "  3. the runtime's model list, and the ROM's asset list\n"
          "  4. git add: a flake only sees tracked files"
          % (a.material, a.material, a.veil_class, a.material, a.material,
             a.tex_size, a.name.replace("_", "-")))
    return 0


# ── selftest ───────────────────────────────────────────────────────────────
def cmd_selftest(a):
    """Prove the offline half. No network, no credits, no writes to assets/."""
    from PIL import Image
    fails = []

    def check(cond, msg):
        print("  %s %s" % ("ok  " if cond else "FAIL", msg))
        if not cond:
            fails.append(msg)

    print("── luminance and the value ladder ──")
    check(abs(luma((0, 0, 0))) < 1e-9, "black is 0.0")
    check(abs(luma((255, 255, 255)) - 1.0) < 1e-9, "white is 1.0")
    ladder, gap = value_ladder([(0, 0, 0), (128, 128, 128), (255, 255, 255)])
    check([round(v, 3) for v, _ in ladder] == [0.0, 0.502, 1.0],
          "a three-step grey ramp ladders 0.00 / 0.50 / 1.00")
    check(abs(gap - 0.498) < 1e-3, "its smallest gap is the lower half")

    print("\n── the value check fires on hue-only separation ──")
    # Two colours a human tells apart instantly and the veil cannot: equal
    # luminance, opposite hue. This is the failure the whole check exists for.
    # 2x2 at --tex-size 2, so nothing is resampled and the two colours reach
    # the palette intact. A 1x1 target would average them into one entry and
    # the check would pass for the wrong reason.
    a_rgb = (255, 0, 0)
    b_rgb = (0, int(round(255 * luma(a_rgb) / 0.7152)), 0)
    hue_only = Image.new("RGB", (2, 2))
    hue_only.putdata([a_rgb, b_rgb, b_rgb, a_rgb])
    _, g = value_ladder([a_rgb, b_rgb])
    check(g < 0.01, "red and an equal-luminance green are %.4f apart" % g)
    try:
        make_ci4(hue_only.copy(), size=2, veil_class="phantom", quiet=True)
        check(False, "make_ci4 refuses a texture made of just those two")
    except SystemExit:
        check(True, "make_ci4 refuses a texture made of just those two")
    kept, _ = make_ci4(hue_only.copy(), size=2, veil_class="phantom",
                       min_gap=MIN_VALUE_GAP, merge=True, quiet=True)
    check(len(set(_pixels(kept.convert("RGB")))) == 1,
          "--merge-by-value collapses them to one entry and passes")

    print("\n── the world class does not own black or white ──")
    ramp = Image.new("RGB", (2, 2))
    ramp.putdata([(0, 0, 0), (80, 40, 40), (170, 90, 90), (255, 255, 255)])
    out, rep = make_ci4(ramp.copy(), size=2, veil_class="world",
                        min_gap=0, quiet=True)
    got = sorted(luma(c) for c in set(_pixels(out.convert("RGB"))))
    check(got[0] >= WORLD_LO - 2e-3 and got[-1] <= WORLD_HI + 2e-3,
          "a black-to-white ramp comes out inside [%.2f, %.2f], got "
          "[%.2f, %.2f]" % (WORLD_LO, WORLD_HI, got[0], got[-1]))
    check(len(got) == 4, "and keeps all four entries distinct (%d)" % len(got))
    plain, _ = make_ci4(ramp.copy(), size=2, veil_class="phantom",
                        min_gap=0, quiet=True)
    flat = sorted(luma(c) for c in set(_pixels(plain.convert("RGB"))))
    check(flat[0] < WORLD_LO and flat[-1] > WORLD_HI,
          "while `phantom` keeps the ends it was given (%.2f, %.2f)"
          % (flat[0], flat[-1]))

    print("\n── power-of-two and squareness are asserted, not assumed ──")
    for bad in (24, 48, 100):
        try:
            make_ci4(ramp, size=bad, veil_class="phantom", quiet=True)
            check(False, "--tex-size %d rejected" % bad)
        except SystemExit:
            check(True, "--tex-size %d rejected" % bad)
    out, _ = make_ci4(ramp, size=32, veil_class="phantom",
                      min_gap=0, quiet=True)
    check(out.size == (32, 32) and out.mode == "P",
          "a 2x2 source comes out 32x32 mode P")

    print("\n── the pass is deterministic ──")
    # A synthetic gradient with far more than 16 colours, ALWAYS — it depends
    # on no repo's assets, so this leg runs anywhere. --fixture adds a second
    # subject rather than replacing this one; a caller passing a fixture is
    # asking for more coverage, not less.
    big = Image.new("RGB", (64, 64))
    big.putdata([((x * 4) % 256, (y * 4) % 256, ((x + y) * 3) % 256)
                 for y in range(64) for x in range(64)])
    subjects = [("a 64x64 synthetic gradient (%d colours)"
                 % len(set(_pixels(big))), big)]
    if a.fixture:
        subjects.append((Path(a.fixture).name, Image.open(a.fixture)))

    for label, fixture in subjects:
        first, r1 = make_ci4(fixture.copy(), size=32, veil_class="phantom",
                             min_gap=0, quiet=True)
        second, r2 = make_ci4(fixture.copy(), size=32, veil_class="phantom",
                              min_gap=0, quiet=True)
        check(_png_bytes(first) == _png_bytes(second),
              "%s quantises to identical bytes twice" % label)
        check(r1 == r2, "  and to an identical report")
        check(r1["colours"] <= CI4_COLOURS,
              "  inside CI4's %d-entry budget (%d colours)"
              % (CI4_COLOURS, r1["colours"]))

    print("\n── re-quantising a conforming texture is a NO-OP ──")
    # The property that matters most for a shared tool: pointed at a texture
    # that is already 32x32 mode P with <=16 colours, this must hand it back
    # unchanged. Without it, running the tool over an existing asset would
    # silently repaint it.
    conforming, _ = to_indexed(Image.merge("RGB", [
        Image.linear_gradient("L").resize((32, 32)).point(
            lambda v, k=k: (v // 32) * 32 + k) for k in (0, 8, 16)]))
    again, rep = make_ci4(conforming.copy(), size=32, veil_class="phantom",
                          min_gap=0, quiet=True)
    check(_png_bytes(again) == _png_bytes(conforming),
          "an already-indexed 32x32 texture round-trips byte-identically")
    if a.fixture and Path(a.fixture).is_file():
        have = Image.open(a.fixture)
        w, h = have.size
        if w != h:
            # Not a failure of this tool — a non-square CI4 texture is the
            # asset that is already wrong. f3d_inject takes ONE `size` and
            # applies it to both S and T, so a 64x32 texture ships with a T
            # wrap mask for a 64-tall image it does not have. Squaring it is
            # the fix, and it necessarily changes the picture, so there is no
            # round-trip to assert.
            print("  note %s is %dx%d, not square. f3d_inject has one `size` "
                  "for S and T, so this one already has a wrong T mask; "
                  "squaring it is a change, not a round-trip."
                  % (Path(a.fixture).name, w, h))
        elif have.mode == "P" and len(set(_pixels(have.convert("RGB")))) \
                <= CI4_COLOURS:
            size = w
            back, _ = make_ci4(have.copy(), size=size, veil_class="phantom",
                               min_gap=0, quiet=True)
            # The property that actually matters: the picture is unchanged.
            # Asserted on PIXELS, not on file bytes, because two palette
            # orderings are both correct and this repo's tree contains both —
            # extract_demon_textures.py sorts its palette, machine_centaur_gen
            # emits mc_*.png in authoring order. Reordering a palette while
            # remapping the indices with it renders identically; it is not a
            # repaint, and failing on it would be this tool objecting to a
            # convention rather than to a defect.
            same_pixels = (_pixels(back.convert("RGB"))
                           == _pixels(have.convert("RGB")))
            check(same_pixels,
                  "%s round-trips to the same PICTURE" % Path(a.fixture).name)
            raw = have.getpalette() or []
            n = max(_pixels(have)) + 1
            stored = [tuple(raw[i * 3:i * 3 + 3]) for i in range(n)]
            sorted_pal = stored == sorted(stored)
            if sorted_pal:
                check(_png_bytes(back) == Path(a.fixture).read_bytes(),
                      "and to its own committed BYTES (its palette is "
                      "already in sorted order)")
            else:
                print("  note %s has an unsorted palette, so the bytes "
                      "change while the picture does not. Both orderings "
                      "are valid; this tool emits sorted."
                      % Path(a.fixture).name)

    print("\n── credentials resolve without touching the network ──")
    os.environ.pop("MESHY_API_KEY", None)
    os.environ["MESHY_API_KEY"] = "msy_selftest_not_a_real_key"
    check(api_key(None) == "msy_selftest_not_a_real_key",
          "$MESHY_API_KEY is read")
    check(api_key("msy_explicit") == "msy_explicit",
          "--api-key wins over the environment")
    del os.environ["MESHY_API_KEY"]

    print()
    if fails:
        for f in fails:
            print("FAIL: %s" % f, file=sys.stderr)
        print("\nmeshy selftest FAILED (%d)" % len(fails), file=sys.stderr)
        return 1
    print("meshy selftest PASSED")
    return 0


def _png_bytes(im):
    import io
    buf = io.BytesIO()
    im.save(buf, "PNG", optimize=False)
    return buf.getvalue()


# ── CLI ────────────────────────────────────────────────────────────────────
def _gen_args(p):
    p.add_argument("--polycount", type=int, default=DEFAULT_POLYCOUNT,
                   help="target faces (Meshy allows 100-300000). Default %d: "
                        "headroom above any console budget, decimated in "
                        "Blender where the ratio is reviewable."
                        % DEFAULT_POLYCOUNT)
    p.add_argument("--topology", default="triangle", choices=("triangle",
                                                              "quad"),
                   help="quad would be re-triangulated on the way in")
    p.add_argument("--pose", default="a-pose",
                   choices=("a-pose", "t-pose", ""),
                   help="only meaningful if the model is ever rigged, and "
                        "free to ask for now")
    p.add_argument("--ai-model", default="latest")
    p.add_argument("--model-type", default=None,
                   choices=(None, "standard", "smart-topology", "lowpoly"))
    p.add_argument("--texture-prompt", default=None)
    p.add_argument("--pbr", action="store_true",
                   help="metallic/roughness/normal maps. F3D has no path for "
                        "any of them; off by default.")
    p.add_argument("--formats", default=["glb"], nargs="+")
    p.add_argument("--out-dir", default=None)


def _ci4_args(p):
    p.add_argument("--tex-size", type=int, default=DEFAULT_TEX_SIZE,
                   help="square, power of two. Default %d (512 B at CI4 "
                        "against a 4 KB TMEM); 64 is 2 KB."
                        % DEFAULT_TEX_SIZE)
    p.add_argument("--veil-class", default="world", choices=VEIL_CLASSES,
                   help="tools/veil_palette.py's value ration. `world` is "
                        "clamped out of true black and true white.")
    p.add_argument("--min-value-gap", type=float, default=MIN_VALUE_GAP,
                   help="reject a palette whose entries are closer than this "
                        "in luminance; 0 disables the check")
    p.add_argument("--merge-by-value", action="store_true",
                   help="collapse too-close pairs instead of failing")


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="meshy.py",
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--api-key", default=None,
                    help="overrides $MESHY_API_KEY and the key file")
    ap.add_argument("--key-file", default=None)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("balance", help="credits left (free; also the "
                                       "credential smoke test)")
    p.set_defaults(fn=cmd_balance)

    p = sub.add_parser("gen", help="text -> 3D (preview then refine)")
    p.add_argument("--name", required=True)
    p.add_argument("--prompt", required=True)
    p.add_argument("--no-refine", action="store_true",
                   help="stop after the untextured preview")
    p.add_argument("--texture-resolution", default="2k",
                   choices=("2k", "4k", "8k"),
                   help="4k/8k is thrown away by a 32x32 target")
    _gen_args(p)
    p.set_defaults(fn=cmd_gen)

    p = sub.add_parser("image", help="image -> 3D (single stage)")
    p.add_argument("--name", required=True)
    p.add_argument("--image", required=True,
                   help="a local file (inlined as a data URI) or a URL")
    _gen_args(p)
    p.set_defaults(fn=cmd_image)

    p = sub.add_parser("status", help="one task, or the account's list")
    p.add_argument("--id", default=None)
    p.add_argument("--kind", default="text-to-3d",
                   choices=("text-to-3d", "image-to-3d"))
    p.add_argument("--limit", type=int, default=20)
    p.set_defaults(fn=cmd_status)

    p = sub.add_parser("fetch", help="re-download a finished task's assets")
    p.add_argument("--id", required=True)
    p.add_argument("--name", default=None)
    p.add_argument("--kind", default="text-to-3d",
                   choices=("text-to-3d", "image-to-3d"))
    p.add_argument("--out-dir", default=None)
    p.set_defaults(fn=cmd_fetch)

    p = sub.add_parser("ci4", help="quantise any image to a CI4-ready "
                                   "indexed PNG (no network)")
    p.add_argument("src")
    p.add_argument("--out", default=None, help="defaults to overwriting src")
    _ci4_args(p)
    p.set_defaults(fn=cmd_ci4)

    p = sub.add_parser("import", help="a download -> tracked assets + a "
                                      "provenance manifest")
    p.add_argument("name")
    p.add_argument("--material", required=True,
                   help="the CANONICAL material name. It is matched by the "
                        "f3d_inject spec, the palette table and the runtime, "
                        "and every one of them fails silently on a typo.")
    p.add_argument("--assets", default="assets")
    p.add_argument("--work-dir", default=None)
    p.add_argument("--archive", default="archives/meshy",
                   help="where the raw drop is parked for provenance; "
                        "'' to skip")
    p.add_argument("--force", action="store_true")
    _ci4_args(p)
    p.set_defaults(fn=cmd_import)

    p = sub.add_parser("selftest", help="prove the offline half; no network")
    p.add_argument("--fixture", default=None,
                   help="an image to round-trip as well as the synthetic one")
    p.set_defaults(fn=cmd_selftest)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
