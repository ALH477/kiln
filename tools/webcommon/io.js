// SPDX-License-Identifier: MIT
//
// io.js — how an editor opens and saves its file, standalone or in Kiln Studio.
//
// Standalone (python -m http.server over tools/) there is nothing to write to,
// so saving is a browser download, as it always was. Inside the studio an
// editor is loaded as /tools/<editor>/index.html?studio=1&file=<repo path> (or
// &model=), and saving is a compare-and-swap: the sha256 of the text that was
// opened travels with the new text, and a file someone else saved in the
// meantime comes back as a conflict (409) instead of being overwritten. While a
// file is open its advisory lock is renewed well inside the server's 30 s
// expiry, so a closed tab or a dropped connection releases it on its own; a
// save by someone else while it is held is a 423 unless they take it over.
//
// No DOM at import time: nix/checks/studio-modules.nix imports this under node.

export function studioParams(search = globalThis.location ? globalThis.location.search : "") {
  const q = new URLSearchParams(search);
  if (q.get("studio") !== "1") return null;
  return { file: q.get("file"), model: q.get("model") };
}

async function call(path, body) {
  const init = { credentials: "same-origin" };
  if (body !== undefined) {
    Object.assign(init, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  }
  const r = await fetch(path, init);
  const raw = await r.text();
  let data = null;
  try {
    data = raw ? JSON.parse(raw) : null;
  } catch {
    data = { error: raw };
  }
  if (!r.ok) {
    const e = new Error((data && data.error) || `${r.status} ${r.statusText}`);
    e.status = r.status;
    e.data = data;
    throw e;
  }
  return data;
}

export class StudioFile {
  constructor(path) {
    this.path = path;
    this.sha = null;
    this.timer = null;
    this.onHide = null;
  }

  // -> { text, lock }. text is null for a file that does not exist yet; saving creates it.
  async open() {
    try {
      const d = await call(`/api/fs/read?path=${encodeURIComponent(this.path)}`);
      this.sha = d.sha256;
      return d;
    } catch (e) {
      if (e.status !== 404) throw e;
      this.sha = null;
      return { path: this.path, text: null, lock: null };
    }
  }

  async save(text, { take = false } = {}) {
    const d = await call("/api/fs/write", { path: this.path, text, baseSha256: this.sha, take });
    this.sha = d.sha256;
    return d;
  }

  // Hold the advisory lock while the editor is open: onState(null) while it is
  // ours, onState(error) while someone else has it.
  hold(onState = () => {}) {
    this.release();
    const renew = () => call("/api/fs/lock", { path: this.path }).then(() => onState(null), (e) => onState(e));
    renew();
    this.timer = setInterval(renew, 10000);
    this.onHide = () => this.release();
    globalThis.addEventListener?.("pagehide", this.onHide);
  }

  release() {
    if (!this.timer) return;
    clearInterval(this.timer);
    this.timer = null;
    globalThis.removeEventListener?.("pagehide", this.onHide);
    fetch("/api/fs/unlock", {
      method: "POST", credentials: "same-origin", keepalive: true,
      headers: { "Content-Type": "application/json" }, body: JSON.stringify({ path: this.path }),
    }).catch(() => {});
  }
}

// Save; if the file is open in someone else's editor, ask before taking it over.
export async function saveOrAsk(file, text, ask = (msg) => globalThis.confirm(msg)) {
  try {
    return await file.save(text);
  } catch (e) {
    if (e.status === 423 && ask(`${e.message}. Save anyway and take the file over?`)) {
      return file.save(text, { take: true });
    }
    throw e;
  }
}

export function describeSaveError(e) {
  if (e.status === 409) return `not saved — ${e.message}. Reload to see their version (export yours first to keep it).`;
  return `not saved — ${e.message}`;
}

export function download(filename, text, type = "text/plain") {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}
