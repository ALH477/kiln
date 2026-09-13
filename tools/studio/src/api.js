// SPDX-License-Identifier: MIT
//
// The studio API, from the page's side. Same-origin fetch with the session
// cookie; the first visit arrives with `#token=…` from the server's startup
// line, which is exchanged for an HttpOnly cookie and then removed from the URL
// so it never sits in history or gets copied into a shared link.

export class AuthError extends Error {}

export async function bootstrap() {
  const m = location.hash.match(/token=([A-Za-z0-9_-]+)/);
  if (!m) return;
  const r = await fetch("/api/session", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ token: m[1] }),
  });
  history.replaceState(null, "", location.pathname + "#/hub");
  if (!r.ok) throw new AuthError("that studio link's token was not accepted");
}

export async function api(path, opts = {}) {
  const r = await fetch(path, {
    credentials: "same-origin",
    ...opts,
    headers: { "Content-Type": "application/json", ...(opts.headers || {}) },
  });
  if (r.status === 401) throw new AuthError("not signed in — open the link the studio printed at startup");
  const text = await r.text();
  const data = text ? JSON.parse(text) : null;
  if (!r.ok) {
    const e = new Error((data && data.error) || r.statusText);
    e.status = r.status;
    e.data = data;
    throw e;
  }
  return data;
}

export const get = (path) => api(path);
export const post = (path, body) => api(path, { method: "POST", body: JSON.stringify(body || {}) });

// A tiny element builder that only ever sets text through textContent: nothing
// the server or a manifest says is parsed as HTML.
export function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === "class") node.className = v;
    else if (k === "text") node.textContent = v;
    else if (k.startsWith("on")) node.addEventListener(k.slice(2), v);
    else if (v === true) node.setAttribute(k, "");
    else if (v !== false && v != null) node.setAttribute(k, v);
  }
  for (const c of children.flat()) {
    if (c == null || c === false) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  return node;
}
