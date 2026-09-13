// SPDX-License-Identifier: MIT
//
// The studio shell: a hash router over panels. A panel is a module exporting
// `render(root, params)` that may return a cleanup function (closing streams).

import { AuthError, bootstrap, el, get } from "./api.js";
import * as hub from "./panels/hub.js";
import * as jobs from "./panels/jobs.js";

const PANELS = { hub, jobs };
const root = document.getElementById("panel");
let cleanup = null;

function parse() {
  const [name = "hub", ...params] = location.hash.replace(/^#\/?/, "").split("/");
  return { name: PANELS[name] ? name : "hub", params };
}

async function show() {
  if (cleanup) { try { cleanup(); } catch { /* a panel's teardown must not block navigation */ } }
  cleanup = null;
  const { name, params } = parse();
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
}

start();
