/* SPDX-License-Identifier: MIT
 *
 * crc_pins.h -- GENERATED, do not hand-edit. The CRC-32/ISO-HDLC of the
 * 196,608-byte RGB888 framebuffer `exs_signaculum_pingue` produces for each
 * view this demo renders, measured on the host tier: the real
 * engine/src/kiln/kiln_soft3d.c module compiled against plat/host and the
 * PINNED engine/src/kiln/gen/signaculum_x86_64.gen.c unit.
 *
 * Regenerate with examples/exsec-signaculum-anim/host_check.c (see its
 * header); the ROM compares against these and reports AGREE/DISAGREE per
 * frame on ISViewer.
 *
 * THE HERO VIEW IS NOT FRAME 0. The committed stream bakes YAW, PITCH =
 * 0.16, -0.14 (EXSECUTOR prototypes/signaculum_mesh.py -- render-logo.py's
 * still), and the animation's frame 0 is yaw 0.0, pitch -0.09. The hero pin
 * below is the SAME constant exsec-signaculum-demo already carries as
 * EXPECTED_CRC, which is what makes this table an extension of that
 * certificate rather than a replacement for it.
 *
 * Inputs these digests are a function of:
 *   filesystem/signaculum.exsg   sha256 07535d3c33f7d72a57db9e901a6cda83210a79d2c56a049e21854e88f958ecf1
 *   filesystem/rotations.bin     sha256 b4787c7ff01fd295f406316d734861c26900ce2e867dab6e04267c2ad0b0f5f7
 */
#ifndef SIGNACULUM_CRC_PINS_H
#define SIGNACULUM_CRC_PINS_H

#include <stdint.h>

#define SIG_ANIM_FRAMES 28u

/* The unpatched stream's view -- exsec-signaculum-demo's EXPECTED_CRC. */
#define SIG_HERO_CRC 0x2025C173u

static const uint32_t sig_frame_crc[SIG_ANIM_FRAMES] = {
    0x5957d4f0u, 0xf1fc44a0u, 0x4bdb8f04u, 0xf4e9f4cbu,
    0xd291937cu, 0x67d7411au, 0x198cfad9u, 0x4bba77c2u,
    0x88fa34b5u, 0x9072a2e0u, 0x50799aaeu, 0x7f9b7bb4u,
    0x5ad9a2d8u, 0xf0619bf9u, 0xf0468585u, 0xf2cb7234u,
    0xc400c96fu, 0x80717395u, 0xcc6f2340u, 0xffc53f40u,
    0xe4c31e77u, 0x16294561u, 0x7609f3d4u, 0xfcb97d55u,
    0xd3cd55d7u, 0xfb783000u, 0x00b8bc9du, 0xce773261u,
};

#endif /* SIGNACULUM_CRC_PINS_H */
