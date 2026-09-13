// SPDX-License-Identifier: MIT
//
// Validate: the repository's validators, runnable on the things they check, and
// the latest report for each — errors that fail, notes that do not, and the
// numbers behind both (tools/schema/report.schema.json).

import { el, get, post } from "../api.js";

export function reportView(report) {
  if (!report) return el("p", { class: "dim", text: "no report" });
  const rows = (items, cls) => items.map((f) =>
    el("tr", {}, el("td", { class: cls, text: f.code }), el("td", { text: f.where || "" }), el("td", { text: f.msg })));
  return el("div", { class: "card" },
    el("div", { class: "row" },
      el("h3", { text: report.tool }),
      el("span", { class: report.ok ? "state-ok" : "state-failed", text: report.ok ? "ok" : `${report.errors.length} error(s)` }),
      report.notes.length ? el("span", { class: "dim", text: `${report.notes.length} note(s)` }) : null,
      report.subject ? el("code", { text: report.subject }) : null),
    (report.errors.length || report.notes.length) ? el("table", {},
      el("tr", {}, ["code", "where", "message"].map((h) => el("th", { text: h }))),
      rows(report.errors, "state-failed"), rows(report.notes, "dim")) : null,
    el("details", {}, el("summary", { text: "metrics" }),
      el("pre", { class: "log", text: JSON.stringify(report.metrics, null, 1) })));
}

export async function render(root) {
  const [{ validators }, { jobs }] = await Promise.all([get("/api/validators"), get("/api/jobs")]);
  const latest = {};
  for (const j of jobs) {
    if (j.kind !== "validate") continue;
    const key = `${j.target} ${j.arg}`;
    if (!latest[key] && ["ok", "failed"].includes(j.state)) latest[key] = j;
  }

  root.replaceChildren(
    el("h1", { text: "Validate" }),
    el("p", { class: "dim", text: "Each validator runs as a job; its report lands here and in the job's log." }),
    validators.map((v) => {
      const select = el("select", {}, v.args.map((a) => el("option", { value: a, text: a })));
      const run = el("button", { class: "primary", onclick: async () => {
        const job = await post("/api/jobs", { kind: "validate", target: v.id, arg: select.value });
        location.hash = `#/jobs/${job.id}`;
      } }, "run");
      run.disabled = v.args.length === 0;
      const done = v.args.map((a) => latest[`${v.id} ${a}`]).filter(Boolean);
      return el("div", {},
        el("h2", { text: v.label }),
        el("div", { class: "row" }, el("code", { text: v.id }), select, run,
          v.args.length === 0 ? el("span", { class: "dim", text: "nothing to validate" }) : null),
        done.map((j) => el("div", {},
          el("div", { class: "dim" }, el("a", { href: `#/jobs/${j.id}`, text: `${j.arg}` }), ` · by ${j.user}`),
          reportView(j.report))));
    }));

  const t = setInterval(() => get("/api/jobs").then((r) => {
    const running = r.jobs.some((j) => j.kind === "validate" && ["queued", "running"].includes(j.state));
    if (!running) return;
  }).catch(() => {}), 4000);
  return () => clearInterval(t);
}
