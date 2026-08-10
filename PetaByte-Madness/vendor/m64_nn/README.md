<!-- SPDX-License-Identifier: MPL-2.0 -->
# m64_nn — N64 Neural Policy Subsystem

Lightweight quantized neural-network inference for NPC AI inside the M64
engine (libdragon + Tiny3D).

Training happens entirely offline on x86. Only the frozen forward pass runs
on the VR4300.

## Design summary

| Constraint              | Choice                                      |
|-------------------------|---------------------------------------------|
| Allocation              | Static only — caller supplies scratch       |
| Preferred math          | int8 weights + int32 accumulators           |
| Fallback                | float32 path for prototyping                |
| libm                    | Avoided on the integer path                 |
| Size target             | Policies of a few tens of KB                |
| Integration             | Actor state → obs → `m64_nn_argmax` → action|

## Files

```
engine/src/m64/
  m64_nn.h          Public API
  m64_nn.c          int8 + float32 kernels

tools/nn/
  train_export.py   Minimal Torch trainer → quantized C header

examples/nn-demo/
  npc_policy_actor.h   Sketch of actor ownership pattern
```

## Quick start (host)

```bash
# Create a tiny discrete policy and emit a C header
python tools/nn/train_export.py \
    --obs 16 --hidden 32 --actions 5 \
    --out examples/nn-demo/policy_chase

# The generated policy_chase.h contains static weight arrays and an
# M64NnLayer table ready for m64_nn_init().
```

Replace the toy supervised loop inside `train_export.py` with a real
PufferLib / CleanRL / PPO loop against a fast simulation of your rooms,
clip world, and actor rules. The export format does not change.

## Runtime usage (console)

```c
#include "m64_nn.h"
#include "policy_chase.h"          /* generated */

static uint8_t nn_scratch[4096];   /* or size with m64_nn_scratch_bytes() */

void npc_init(/* ... */) {
    M64NnModel model;
    m64_nn_init(&model, M64_NN_DTYPE_INT8,
                3, policy_chase_layers,
                16, 5, NULL,
                nn_scratch, sizeof(nn_scratch));
}

void npc_update(/* ... */) {
    float obs_f[16];
    /* fill from position, health, target angle, flags, … */

    int8_t obs[16];
    m64_nn_pack_obs_i8(obs, 16, obs_f);

    int action = m64_nn_argmax(&model, obs);
    /* map action → velocity / event / dialogue token */
}
```

See `examples/nn-demo/npc_policy_actor.h` for a compact ownership pattern
you can drop into an `M64Actor` state block.

## Recommended training stack

1. **PufferLib** (or CleanRL) on x86 for high-throughput rollouts.
2. Environment = a fast C or Python reimplementation of the relevant M64
   rules (`m64_clip`, rooms, simple physics).
3. Keep the policy deliberately tiny (≤ 2–3 layers, width 16–64).
4. Post-training quantization → C header (or later a StreamDB blob).
5. Golden vectors from the host must match the console integer path
   bit-for-bit.

## Integration into the M64 tree

1. Copy `m64_nn.h` / `m64_nn.c` into `engine/src/m64/`.
2. Add the two files to the engine library build
   (`nix/engine.nix` / the engine Makefile).
3. Train + export a policy.
4. `#include` the generated header from the NPC profile.
5. Build observation vectors from real signals (distance, visibility,
   health, Z-target state, room id, surface, …).

## Future extensions (ordered by value)

- GRU / MinGRU cell (int8) for short-term temporal memory.
- `m64_asset_policy` that loads a quantized blob from StreamDB.
- RSP matvec microcode (already demonstrated by 64GPT) for wider nets.
- Quantization-aware training so the int8 model is the one optimized.

## Licence

MPL-2.0
