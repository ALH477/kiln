// SPDX-License-Identifier: MIT
//
// Game: a browser build playing in this tab, with the engine's own numbers
// beside it. The page is the build's own (plat/shell/kiln_web_shell.html),
// served same-origin under /play/<package>/. It posts what the launcher knows —
// frames presented, the pad it pushed, the console's log — and takes console
// commands the same way (plat/shell/shell_web.c's studio bridge). Everyone runs
// their own copy of the wasm; nothing is streamed between people.

import { el, get, post } from "../api.js";

// Bit order of Module.kiln.pad.buttons (shell_web.c, web_publish_pad).
const BUTTONS = ["A", "B", "Z", "L", "R", "Start", "C↑", "C↓", "C←", "C→", "D↑", "D↓", "D←", "D→"];

export async function render(root, params) {
  const pkg = decodeURIComponent(params[0] || "");
  if (!/^web-[A-Za-z0-9._+-]+$/.test(pkg)) throw new Error(`not a browser build: ${pkg}`);
  const manifest = await get("/api/manifest");
  if ((manifest.packages[pkg] || {}).kind !== "web") throw new Error(`${pkg} is not a browser build in the manifest`);
  const pc = `pc-${pkg.slice(4)}`;
  const hasPc = (manifest.packages[pc] || {}).kind === "pc";

  let alive = true;
  let iframe = null;
  const stage = el("div", { class: "game-stage" });
  const stats = el("pre", { class: "log", text: "not running" });
  const consoleLog = el("pre", { class: "log game-console" });
  const cmd = el("input", { placeholder: "console command — try help", maxlength: "63" });
  const shotBox = el("div", {});
  const send = (msg) => iframe && iframe.contentWindow && iframe.contentWindow.postMessage(msg, location.origin);

  const onMessage = (e) => {
    if (!iframe || e.origin !== location.origin || e.source !== iframe.contentWindow || !e.data) return;
    const m = e.data;
    if (m.kiln === "state") {
      const f = m.frame || {};
      const p = m.pad || {};
      const held = BUTTONS.filter((_, i) => (p.buttons || 0) & (1 << i)).join(" ") || "—";
      stats.textContent = [`frame ${f.n}   ${f.w}x${f.h}`,
        ...Object.entries(f.counters || {}).map(([k, v]) => `${k.padEnd(14)} ${v}`),
        `stick ${p.stick_x || 0},${p.stick_y || 0}   ${held}`].join("\n");
      if (Array.isArray(m.console)) {
        consoleLog.textContent = m.console.join("\n");
        consoleLog.scrollTop = consoleLog.scrollHeight;
      }
    } else if (m.kiln === "shot" && typeof m.png === "string" && m.png.startsWith("data:image/png;base64,")) {
      shotBox.replaceChildren(el("img", { src: m.png, class: "game-shot", alt: `${pkg}, the frame it drew` }),
        el("a", { href: m.png, download: `${pkg}.png`, text: "save png" }));
    }
  };
  window.addEventListener("message", onMessage);

  const play = () => {
    iframe = el("iframe", { class: "game-frame", src: `/play/${pkg}/index.html`, title: pkg, allow: "gamepad; autoplay" });
    iframe.addEventListener("load", () => iframe.focus());
    stage.replaceChildren(iframe);
  };

  const buildAndPlay = async () => {
    stage.replaceChildren(el("p", { class: "dim", text: `building ${pkg}…` }));
    const job = await post("/api/jobs", { kind: "build", target: pkg });
    stage.append(el("a", { href: `#/jobs/${job.id}`, text: "build log" }));
    for (;;) {
      if (!alive) return;
      const j = await get(`/api/jobs/${job.id}`);
      if (j.state === "ok") return play();
      if (["failed", "cancelled"].includes(j.state)) {
        return stage.replaceChildren(el("p", { class: "err" }, `build ${j.state} — `,
          el("a", { href: `#/jobs/${job.id}`, text: "log" })));
      }
      await new Promise((r) => setTimeout(r, 1000));
    }
  };

  cmd.addEventListener("keydown", (e) => {
    if (e.key !== "Enter" || !cmd.value.trim()) return;
    send({ cmd: cmd.value.trim() });
    cmd.value = "";
  });

  root.replaceChildren(
    el("div", { class: "row" }, el("h1", { text: pkg }),
      el("button", { class: "primary", onclick: buildAndPlay }, "build & play"),
      el("button", { onclick: () => send({ shot: true }) }, "screenshot"),
      hasPc ? el("button", {
        title: `${pc}: 120 frames headless, the real renderer, no window`,
        onclick: async () => { location.hash = `#/jobs/${(await post("/api/jobs", { kind: "host-shot", target: pc })).id}`; },
      }, "host shot") : null),
    el("div", { class: "game" }, stage,
      el("div", { class: "game-side" },
        el("h2", { text: "Engine" }), stats,
        el("h2", { text: "Console" }), consoleLog, cmd,
        shotBox)));

  const { ready } = await get(`/api/play/${pkg}`);
  if (ready) play();
  else stage.replaceChildren(el("p", { class: "dim", text: "Not built in this session yet — build & play." }));
  return () => {
    alive = false;
    window.removeEventListener("message", onMessage);
  };
}
