// SPDX-License-Identifier: MIT
//
// Files: what the editors can open (tools/studio/allow.json), who has each one
// open, and a way in. A save through the studio never overwrites a version its
// author did not see, and the file's validator runs after every save.

import { el, get } from "../api.js";

function editorLink(f) {
  if (f.editor === "map") return `#/map/${f.path}`;
  if (f.editor === "pose") return `#/pose/${f.path.split("/").pop().split(".")[0]}`;
  return null;
}

export async function render(root) {
  const name = el("input", { placeholder: "new level name", pattern: "[a-z0-9_-]+" });
  const create = el("button", { onclick: () => {
    if (!/^[a-z0-9_-]+$/.test(name.value)) return name.focus();
    location.hash = `#/map/assets/${name.value}.map`;
  } }, "new map");
  const list = el("div", {});
  root.replaceChildren(
    el("h1", { text: "Files" }),
    el("p", { class: "dim", text: "Saving through the studio refuses to overwrite a version you did not see, and runs the file's validator." }),
    el("div", { class: "row" }, name, create),
    list);

  const draw = async () => {
    const [{ files }, { people }] = await Promise.all([get("/api/fs/list"), get("/api/presence")]);
    const on = (path) => [...new Set(people.filter((p) => p.file === path).map((p) => p.user))];
    list.replaceChildren(
      el("table", {},
        el("tr", {}, ["file", "validator", "lock", "open here"].map((h) => el("th", { text: h }))),
        files.map((f) => {
          const href = editorLink(f);
          return el("tr", {},
            el("td", {}, href ? el("a", { href, text: f.path }) : el("code", { text: f.path })),
            el("td", { class: "dim", text: f.validator || "" }),
            el("td", { class: f.lock ? "state-running" : "dim", text: f.lock ? f.lock.user : "" }),
            el("td", { text: on(f.path).join(", ") }));
        })));
  };
  await draw();
  const t = setInterval(() => draw().catch(() => {}), 5000);
  return () => clearInterval(t);
}
