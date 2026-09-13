// SPDX-License-Identifier: MIT
//
// The studio shell: a hash router over panels. A panel is a module exporting
// `render(root, params)` that may return a cleanup function (closing streams).
// The shell also keeps this tab's presence — which panel, which file — beating,
// and shows everyone else's.

import { AuthError, bootstrap, el, get, post } from "./api.js";
import * as editor from "./panels/editor.js";
import * as files from "./panels/files.js";
import * as game from "./panels/game.js";
import * as hub from "./panels/hub.js";
import * as jobs from "./panels/jobs.js";
import * as validate from "./panels/validate.js";

const PANELS = { hub, jobs, validate, files, game, map: editor.map, pose: editor.pose };
const root = document.getElementById("panel");
let cleanup = null;

// getRandomValues, not randomUUID: the latter needs a secure context, and a
// tailnet session behind `tailscale serve --http` is not one.
const CLIENT = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, "0")).join("");
let here = { panel: "hub", file: null };

function fileOf(name, params) {
  if (name === "map") return params.map(decodeURIComponent).join("/");
  if (name === "pose" && params[0]) return `tools/poser/data/${decodeURIComponent(params[0])}`;
  return null;
}

async function beat() {
  try {
    const { people } = await post("/api/presence", { client: CLIENT, ...here });
    const seen = new Set();
    document.getElementById("people").replaceChildren(...people.filter((p) => {
      const key = `${p.user} ${p.panel} ${p.file}`;
      return !seen.has(key) && seen.add(key);
    }).map((p) => el("div", { class: "person" }, el("b", { text: p.user }), ` · ${p.panel}`,
      p.file ? el("div", { class: "dim", text: p.file }) : null)));
  } catch { /* presence is best-effort; the panel itself reports real errors */ }
}

function parse() {
  const [name = "hub", ...params] = location.hash.replace(/^#\/?/, "").split("/");
  return { name: PANELS[name] ? name : "hub", params };
}

async function show() {
  if (cleanup) { try { cleanup(); } catch { /* a panel's teardown must not block navigation */ } }
  cleanup = null;
  const { name, params } = parse();
  here = { panel: name, file: fileOf(name, params) };
  beat();
  for (const a of document.querySelectorAll("#nav a")) a.classList.toggle("active", a.dataset.panel === name);
  root.replaceChildren(el("p", { class: "dim", text: "loading…" }));
  try {
    cleanup = await PANELS[name].render(root, params);
  } catch (e) {
    root.replaceChildren(el("p", { class: "err", text: e instanceof AuthError ? e.message : `error: ${e.message}` }));
  }
}

async function start() {
  try {
    await bootstrap();
    const who = await get("/api/whoami");
    document.getElementById("who").textContent = `${who.user} · ${who.via}`;
  } catch (e) {
    root.replaceChildren(el("p", { class: "err", text: e.message }));
    return;
  }
  window.addEventListener("hashchange", show);
  show();
  setInterval(beat, 10000);
}

start();
