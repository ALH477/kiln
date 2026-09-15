# nix/demos — per-demo packages, one file per example

Every `*.nix` file in this directory is imported by `flake.nix` and its attribute
set is merged into `packages`. A demo keeps its **extra** packages here — jump
ROMs from `mkJumpRoms` and `pc-*` / `web-*` host builds — so that work on two
demos never touches the same lines of `flake.nix`. The base ROM itself stays in
`flake.nix` as `<demo>Args` + `<demo> = mkN64Rom <demo>Args;`, because the check
lists and `rom-*` checks refer to it by name.

Each file is a function of one argument, the context `flake.nix` passes:

```nix
ctx: with ctx; {
  # jump ROMs: <demo>-<jump>, KILN_JUMP=<JUMP> baked in
} // mkJumpRoms args.texanimDemoArgs [ "SCROLL" ] // {
  pc-texanim-demo = hostNative.mkGame {
    pname = "kiln-texanim-demo";
    sources = [ ../../examples/texanim-demo/main.c ];
    assets = args.texanimDemoArgs.assets or [ ];
  };
}
```

`ctx` carries `pkgs`, `lib`, `mkN64Rom`, `mkJumpRoms`, `hostNative`, `hostWasm`,
`assetLib`, `blenderLib`, the shared asset derivations (`demoSound`,
`stepSound`, `textures`, `testModels`, the character models, …) and `args`, an
attribute set of every converted demo's `<demo>Args`. Paths inside a file are
relative to this directory (`../../examples/...`, `../../assets/...`).

A new asset a demo needs can be defined in its own file and added to the ROM by
editing that demo's `<demo>Args.assets` in `flake.nix`; or, if the asset must be
in the base ROM's args, define it in `flake.nix` next to the args block.
