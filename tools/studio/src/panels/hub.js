// SPDX-License-Identifier: MIT
//
// The hub: every game the flake builds, from the generated manifest, with its
// ROM, jump ROMs and host builds, and the checks — each one a job away.

import { el, get, post } from "../api.js";

async function start(kind, target) {
  const job = await post("/api/jobs", { kind, target });
  location.hash = `#/jobs/${job.id}`;
}

function buildButton(label, pkg, primary = false) {
  return el("button", { class: primary ? "primary" : "", title: `nix build .#${pkg}`, onclick: () => start("build", pkg) }, label);
}

function gameCard(name, g) {
  const jumps = g.jumps.map((j) => buildButton((j.jump || j.package).toLowerCase(), j.package));
  const hosts = [
    ...g.web.map((h) => el("button", { class: "primary", title: "play it in this tab",
      onclick: () => { location.hash = `#/game/${h.package}`; } }, `play ${h.package}`)),
    ...g.pc.map((h) => el("span", { class: "row" }, buildButton(h.package, h.package),
      el("button", { title: "run it headless and keep the frame it drew", onclick: () => start("host-shot", h.package) }, "shot"))),
  ];
  return el("div", { class: "card" },
    el("div", { class: "row" },
      el("h3", { text: name }),
      g.hostable ? el("span", { class: "badge host", text: "runs on host" }) : el("span", { class: "badge", text: "console only" })),
    el("div", { class: "dim", text: g.romTitle || "" }),
    el("div", { class: "chips" }, g.roms.map((r) => buildButton(`build ${r}`, r, true))),
    jumps.length ? el("details", {}, el("summary", { text: `${jumps.length} jump ROM${jumps.length > 1 ? "s" : ""}` }),
      el("div", { class: "chips" }, jumps)) : null,
    hosts.length ? el("details", {}, el("summary", { text: `${hosts.length} host build${hosts.length > 1 ? "s" : ""}` }),
      el("div", { class: "chips" }, hosts)) : null);
}

export async function render(root) {
  let manifest;
  try {
    manifest = await get("/api/manifest");
  } catch (e) {
    if (e.status === 503) {
      root.replaceChildren(el("h1", { text: "Hub" }),
        el("p", { class: "dim", text: "Evaluating the flake's studioManifest… (this takes a moment the first time)" }),
        e.data && e.data.detail ? el("pre", { class: "log", text: e.data.detail }) : null);
      const t = setTimeout(() => render(root), 3000);
      return () => clearTimeout(t);
    }
    throw e;
  }
  const caps = await get("/api/caps").catch(() => ({}));
  const games = Object.entries(manifest.games).sort(([a], [b]) => a.localeCompare(b));

  root.replaceChildren(
    el("h1", { text: "Hub" }),
    el("div", { class: "row dim" },
      `${games.length} games · ${Object.keys(manifest.packages).length} packages · ${manifest.checks.length} checks · ${manifest.system}`,
      el("button", { onclick: () => post("/api/manifest/reload").then(() => render(root)) }, "reload manifest")),
    el("h2", { text: "Machine" }),
    capsRow(caps),
    el("h2", { text: "Checks" }),
    el("div", { class: "row" },
      el("button", { class: "primary", onclick: () => start("cheap", "") }, `run the cheap checks (${manifest.cheap.length})`)),
    el("details", {}, el("summary", { text: `all ${manifest.checks.length} checks` }),
      el("div", { class: "chips" }, manifest.checks.map((c) => el("button", { onclick: () => start("check", c) }, c)))),
    el("h2", { text: "Games" }),
    el("div", { class: "grid" }, games.map(([name, g]) => gameCard(name, g))));
}

function capsRow(caps) {
  if (caps.loading) return el("p", { class: "dim", text: "detecting capabilities…" });
  if (caps.error) return el("p", { class: "err", text: `capability detection failed: ${caps.error}` });
  const keys = ["toolchain", "ares", "hyprland", "grim", "blender", "chromium", "sc64", "ed64", "tailscale"];
  return el("div", { class: "chips" },
    keys.filter((k) => k in caps).map((k) =>
      el("span", { class: "badge" + (caps[k] ? " host" : ""), text: `${caps[k] ? "✓" : "✗"} ${k}` })),
    el("span", { class: "badge", text: caps.local_client ? "this machine" : "remote client" }));
}
