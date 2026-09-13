// SPDX-License-Identifier: MIT
//
// Jobs: everything anyone in the session has run, and one job's live log.
// The log is a server-sent event stream; EventSource reconnects on its own and
// resumes from the last line id, so a dropped connection neither repeats nor
// loses lines.

import { el, get, post } from "../api.js";
import { reportView } from "./validate.js";

const fmtTime = (t) => (t ? new Date(t * 1000).toLocaleTimeString() : "");
const duration = (j) => (j.started ? `${Math.round(((j.ended || Date.now() / 1000) - j.started))} s` : "");

export async function render(root, params) {
  return params[0] ? detail(root, params[0]) : list(root);
}

async function list(root) {
  const draw = async () => {
    const { jobs } = await get("/api/jobs");
    root.replaceChildren(
      el("h1", { text: "Jobs" }),
      jobs.length === 0 ? el("p", { class: "dim", text: "Nothing has run yet. Start a build from the hub." }) :
        el("table", {},
          el("tr", {}, ["when", "job", "target", "who", "state", "time"].map((h) => el("th", { text: h }))),
          jobs.map((j) => el("tr", {},
            el("td", { text: fmtTime(j.created) }),
            el("td", {}, el("a", { href: `#/jobs/${j.id}`, text: j.kind })),
            el("td", {}, el("code", { text: j.target || "—" })),
            el("td", { text: j.user }),
            el("td", { class: `state-${j.state}`, text: j.state }),
            el("td", { text: duration(j) })))));
  };
  await draw();
  const t = setInterval(() => draw().catch(() => {}), 2000);
  return () => clearInterval(t);
}

async function detail(root, id) {
  const job = await get(`/api/jobs/${id}`);
  const state = el("span", { class: `state-${job.state}`, text: job.state });
  const outputs = el("div", { class: "chips" });
  const report = el("div", {});
  const log = el("pre", { class: "log" });
  const cancel = el("button", { onclick: () => post(`/api/jobs/${id}/cancel`) }, "cancel");
  cancel.disabled = ["ok", "failed", "cancelled"].includes(job.state);

  root.replaceChildren(
    el("div", { class: "row" }, el("h1", { text: `${job.kind} ${job.target || ""}` }), state, cancel),
    el("div", { class: "dim" }, `started by ${job.user} at ${fmtTime(job.created)} · `, el("code", { text: job.argv.join(" ") })),
    outputs,
    report,
    log);

  const es = new EventSource(`/api/jobs/${id}/events`);
  es.addEventListener("line", (ev) => {
    const stick = log.scrollTop + log.clientHeight >= log.scrollHeight - 8;
    log.append(JSON.parse(ev.data) + "\n");
    if (stick) log.scrollTop = log.scrollHeight;
  });
  es.addEventListener("done", (ev) => {
    const snap = JSON.parse(ev.data);
    state.className = `state-${snap.state}`;
    state.textContent = snap.state;
    cancel.disabled = true;
    outputs.replaceChildren(...snap.outputs.map((o) => el("code", { text: o })));
    if (snap.report) report.replaceChildren(reportView(snap.report));
    if (snap.shot) report.replaceChildren(el("img", { src: `/api/jobs/${id}/shot.png`, class: "game-shot", alt: "the frame it drew" }));
    es.close();
  });
  return () => es.close();
}
