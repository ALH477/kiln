/* SPDX-License-Identifier: MIT
 *
 * host_hooks.c — the one mutable thing in the host backend.
 *
 * Everything else under plat/host/src is a pure function of the calls the
 * engine makes: same calls, same pixels, on every machine. That is what lets
 * nix/checks/ compare a PNG byte for byte. A window, a clock and a speaker are
 * all the opposite of that, so they do not live in the backend — they are
 * installed into it by a launcher, and the backend's default is to have none.
 *
 * Keeping the table here rather than in host_gfx.c is what stops a launcher
 * from being a second implementation of the renderer: plat/shell may set these
 * five pointers and may call kiln_host_pad_set, and that is the entire surface
 * it is allowed. See plat/shell/kiln_shell.c.
 */
#include <kiln_host.h>
#include <string.h>

static KilnHostHooks g_hooks;

void kiln_host_set_hooks(const KilnHostHooks *hooks)
{
    if (hooks) g_hooks = *hooks;
    else       memset(&g_hooks, 0, sizeof g_hooks);
}

const KilnHostHooks *kiln_host_hooks(void) { return &g_hooks; }
