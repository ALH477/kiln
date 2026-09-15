// SPDX-License-Identifier: MIT
//
// Agents: the CrewAI crew's surface in the studio. A person writes a brief;
// the studio creates the worktree and hands the flow to the agents container;
// the flow patches its record as it runs. A task awaiting a human shows
// approve / reject; an approved, finished one shows merge (fast-forward or
// refuse) — the studio merges, never the agent.

import { el, get, post } from "../api.js";

const fmtTime = (t) => (t ? new Date(t * 1000).toLocaleString() : "");

export async function render(root, params) {
  return params[0] ? detail(root, params[0]) : list(root);
}

async function list(root) {
  const brief = el("textarea", { rows: 3, placeholder: "a task for the crew — e.g. “add a pillar to assets/map_demo.map”", style: "width:100%" });
  const status = el("div", { class: "dim", text: "" });
  const submit = el("button", {
    onclick: async () => {
      submit.disabled = true;
      try {
        const task = await post("/api/agent-tasks", { brief: brief.value });
        location.hash = `#/agents/${task.id}`;
      } catch (e) {
        status.textContent = e.message;
        submit.disabled = false;
      }
    },
  }, "start task");

  const draw = async () => {
    const { tasks, agents_service } = await get("/api/agent-tasks");
    listBody.replaceChildren(
      agents_service ? null : el("p", { class: "warn", text: "no agents service on the network (KILN_AGENTS_URL unset) — tasks queue as worktrees until the agents container is up." }),
      tasks.length === 0 ? el("p", { class: "dim", text: "No tasks yet." }) :
        el("table", {},
          el("tr", {}, ["when", "task", "who", "phase", "decision"].map((h) => el("th", { text: h }))),
          tasks.map((t) => el("tr", {},
            el("td", { text: fmtTime(t.created) }),
            el("td", {}, el("a", { href: `#/agents/${t.id}`, text: t.brief.slice(0, 80) })),
            el("td", { text: t.by }),
            el("td", { class: `state-${t.phase === "done" ? "ok" : "running"}`, text: t.phase || "" }),
            el("td", { text: t.approved === true ? `approved by ${t.decided_by}` :
                             t.approved === false ? `rejected by ${t.decided_by}` : "—" })))));
  };
  const listBody = el("div", {});
  root.replaceChildren(el("h1", { text: "Agent tasks" }), brief, submit, status, listBody);
  await draw();
  const t = setInterval(() => draw().catch(() => {}), 3000);
  return () => clearInterval(t);
}

async function detail(root, id) {
  const errBox = el("p", { class: "err", text: "" });
  const err = (msg) => { errBox.textContent = msg; };
  const draw = async () => {
    const t = await get(`/api/agent-tasks/${id}`);
    const actions = el("div", { class: "chips" });
    if (t.phase === "awaiting-human" && t.approved === null) {
      actions.replaceChildren(
        el("button", { onclick: () => post(`/api/agent-tasks/${id}/approve`).catch((e) => err(e.message)) }, "approve"),
        el("button", { onclick: () => post(`/api/agent-tasks/${id}/reject`).catch((e) => err(e.message)) }, "reject"));
    } else if (t.phase === "done" && t.approved === true && !t.merged) {
      actions.replaceChildren(
        el("button", { onclick: () => post(`/api/agent-tasks/${id}/merge`).catch((e) => err(e.message)) }, "merge (fast-forward)"),
        el("button", { onclick: () => post(`/api/agent-tasks/${id}/discard`).catch((e) => err(e.message)) }, "discard"));
    } else if (["queued", "running"].includes(t.phase) || t.approved === false) {
      actions.replaceChildren(
        el("button", { onclick: () => post(`/api/agent-tasks/${id}/discard`).catch((e) => err(e.message)) }, "discard"));
    } else {
      actions.replaceChildren();
    }
    errBox.textContent = t.note || "";
    body.replaceChildren(
      el("div", { class: "row" }, el("h1", { text: `task ${t.id}` }),
         el("span", { class: `state-${t.phase === "done" || t.phase === "merged" ? "ok" : "running"}`, text: t.phase })),
      el("p", {}, el("b", { text: t.brief }), ` — ${t.by}, ${fmtTime(t.created)}`),
      t.kind ? el("p", { class: "dim", text: `kind: ${t.kind} · branch ${t.branch}` }) : el("p", { class: "dim", text: `branch ${t.branch}` }),
      actions,
      t.plan ? el("h2", { text: "plan" }) : null,
      t.plan ? el("pre", { class: "log", text: t.plan }) : null,
      t.validations ? el("h2", { text: "validations" }) : null,
      t.validations ? el("pre", { class: "log", text: JSON.stringify(t.validations, null, 2) }) : null,
      t.review ? el("h2", { text: "review" }) : null,
      t.review ? el("pre", { class: "log", text: t.review }) : null,
      t.diff_stat ? el("h2", { text: "diff" }) : null,
      t.diff_stat ? el("pre", { class: "log", text: t.diff_stat + (t.ahead ? "\n\n" + t.ahead : "") }) : null,
      t.report ? el("h2", { text: "final report" }) : null,
      t.report ? el("pre", { class: "log", text: JSON.stringify(t.report, null, 2) }) : null);
  };
  const body = el("div", {});
  root.replaceChildren(el("a", { href: "#/agents", text: "← all tasks" }), errBox, body);
  await draw();
  const t = setInterval(() => draw().catch(() => {}), 3000);
  return () => clearInterval(t);
}
