// SPDX-License-Identifier: MIT
//
// The editors inside the studio: the map maker and the poser, same-origin in an
// iframe and opened on one file. They are the standalone apps; ?studio=1 is what
// makes tools/webcommon/io.js save through the studio instead of downloading.

import { el } from "../api.js";

function frame(root, title, subject, src) {
  root.replaceChildren(
    el("div", { class: "row" }, el("h1", { text: title }), el("code", { text: subject }),
      el("a", { href: "#/files", text: "all files" })),
    el("iframe", { class: "editor-frame", src, title }));
}

export const map = {
  render(root, params) {
    const path = params.map(decodeURIComponent).join("/");
    if (!/^assets\/[A-Za-z0-9_.+-]+\.map$/.test(path)) throw new Error(`not a level under assets/: ${path}`);
    frame(root, "Map", path, `/tools/mapmaker/index.html?studio=1&file=${encodeURIComponent(path)}`);
  },
};

export const pose = {
  render(root, params) {
    const model = decodeURIComponent(params[0] || "dank");
    if (!/^[a-z0-9_-]+$/.test(model)) throw new Error(`not a model name: ${model}`);
    frame(root, "Poser", model, `/tools/poser/index.html?studio=1&model=${encodeURIComponent(model)}`);
  },
};
