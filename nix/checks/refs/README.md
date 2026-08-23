<!-- SPDX-License-Identifier: MIT -->
# Reference captures

Committed output of the host 2D pass, compared byte-for-byte by
`nix/checks/kiln-gui.nix`. This is the gate CLAUDE.md says cannot exist —
*"screenshot verification is not part of `nix flake check`— it needs a live
Wayland session"* — and it exists because the host renderer is software, so it
needs no session, no driver and no compositor.

Two files per frame, because they fail differently and that is the point:

- **`*.png`** — the framebuffer. Catches geometry: a panel that moved, a bar
  that fills the wrong way, a line quad with a broken winding.
- **`*.txt`** — the text manifest, one line per `rdpq_text_print`: position,
  colour, measured width, string. Catches the thing a pixel diff reports
  worst. A one-pixel baseline shift lights up every glyph in a PNG diff and
  tells you only "text changed"; the manifest says *which label moved and to
  where*.

Both are byte-stable across runs and machines (see `plat/host/src/host_png.c`
on why the encoder is deliberately boring). If a change is intended, look at
the diff, then regenerate:

```
nix build .#checks.x86_64-linux.kiln-gui   # fails, and prints what differs
# ... then run the harness by hand and copy its output over these files
```

**A reference image is only as good as someone having looked at it.** Before
committing a new one, magnify it and check it reads correctly — the whole
value here is that the picture is right, and a gate is perfectly happy to
freeze a wrong picture forever.
