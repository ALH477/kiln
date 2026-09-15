// SPDX-License-Identifier: MIT
//
// modules_check.mjs — every ES module the studio and its editors load resolves,
// parses, and exports the names imported from it.
//
// Nothing else loads these files outside a browser. mapmaker-roundtrip imports
// mapio.js and nothing more, which is how a spawn selection that threw a
// ReferenceError on every click once shipped: no gate had ever loaded main.js.
// There is no browser in the sandbox, so this does what a browser does first —
// follows each page's entry script through the page's own importmap, file by
// file — and fails on a missing file, a missing export, a syntax error, a bare
// specifier the importmap does not cover, or an editor using a root-absolute
// path (editors are served at /mapmaker/ standalone and /tools/mapmaker/ in the
// studio, so only relative paths work in both). Then the modules that touch no
// DOM when loaded are really imported, with the importmap installed as a
// resolve hook.
//
//   node tools/studio/tests/modules_check.mjs <tools dir>

import { copyFileSync, existsSync, mkdtempSync, readFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { register } from "node:module";
import { tmpdir } from "node:os";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const tools = resolve(process.argv[2] || join(dirname(fileURLToPath(import.meta.url)), "../.."));
const problems = [];
const fail = (msg) => problems.push(msg);
const rel = (f) => relative(tools, f);
const isVendor = (f) => rel(f).split("/").includes("vendor");

// `root`: the directory a page's "/" means. Only the studio has one.
const PAGES = [
  { page: "studio/index.html", root: "studio" },
  { page: "mapmaker/index.html" },
  { page: "poser/index.html" },
];
const MUST_VISIT = ["studio/src/shell.js", "mapmaker/src/main.js", "poser/src/main.js", "webcommon/io.js",
  "webcommon/viewport.js"];
const IMPORTABLE = ["mapmaker/src/mapio.js", "mapmaker/src/snap.js", "mapmaker/src/vocab.gen.js",
  "poser/src/pose.js", "poser/src/lag.gen.js", "poser/src/gltf.js", "webcommon/io.js", "webcommon/viewport.js"];

function stripComments(src) {
  return src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
}

function exportsOf(src) {
  const names = new Set();
  let star = false;
  for (const m of src.matchAll(/\bexport\s+(?:async\s+)?(?:function\*?|class|const|let|var)\s+([\w$]+)/g)) names.add(m[1]);
  for (const m of src.matchAll(/\bexport\s*\{([^}]*)\}/g)) {
    for (const part of m[1].split(",").map((s) => s.trim()).filter(Boolean)) {
      const [name, alias] = part.split(/\s+as\s+/);
      names.add((alias || name).trim());
    }
  }
  if (/\bexport\s+default\b/.test(src)) names.add("default");
  if (/\bexport\s*\*\s*from\b/.test(src)) star = true;
  return { names, star };
}

function importsOf(src) {
  const out = [];
  const re = /(?:^|[;\n])\s*(import|export)\s*(?:([\w$*\s{},]*?)\s*from\s*)?["']([^"']+)["']/g;
  for (const m of src.matchAll(re)) {
    const clause = m[2] || "";
    const names = [];
    const braces = clause.match(/\{([^}]*)\}/);
    if (braces) {
      for (const part of braces[1].split(",").map((s) => s.trim()).filter(Boolean)) names.push(part.split(/\s+as\s+/)[0].trim());
    }
    if (m[1] === "import" && /^\s*[\w$]+\s*(,|$)/.test(clause)) names.push("default");
    out.push({ spec: m[3], names });
  }
  for (const m of src.matchAll(/\bimport\(\s*["']([^"']+)["']\s*\)/g)) out.push({ spec: m[1], names: [] });
  return out;
}

function importmapOf(html) {
  const m = html.match(/<script type="importmap">([\s\S]*?)<\/script>/);
  return m ? JSON.parse(m[1]).imports || {} : {};
}

function bestKey(map, spec) {
  let best = null;
  for (const key of Object.keys(map)) {
    if ((key === spec || (key.endsWith("/") && spec.startsWith(key))) && (!best || key.length > best.length)) best = key;
  }
  return best;
}

function resolveUrl(spec, fromFile, page) {
  if (spec.startsWith("/")) {
    if (!page.root) {
      fail(`${rel(fromFile)}: '${spec}' is root-absolute, and an editor is served under two different prefixes`);
      return null;
    }
    return join(tools, page.root, spec);
  }
  return resolve(dirname(fromFile), spec);
}

function resolveSpec(spec, fromFile, page) {
  if (spec.startsWith("./") || spec.startsWith("../") || spec.startsWith("/")) return resolveUrl(spec, fromFile, page);
  const key = bestKey(page.map, spec);
  if (!key) {
    fail(`${rel(fromFile)}: bare specifier '${spec}' is not in ${page.page}'s importmap`);
    return null;
  }
  return resolveUrl(page.map[key] + spec.slice(key.length), join(tools, page.page), page);
}

const checkedSyntax = new Set();
const scratch = mkdtempSync(join(tmpdir(), "studio-modules-"));
function syntax(file) {
  if (checkedSyntax.has(file)) return;
  checkedSyntax.add(file);
  const copy = join(scratch, `${checkedSyntax.size}.mjs`);
  copyFileSync(file, copy);
  const r = spawnSync(process.execPath, ["--check", copy], { encoding: "utf8" });
  if (r.status !== 0) fail(`${rel(file)}: does not parse — ${(r.stderr || "").split("\n").find((l) => /Error/.test(l)) || r.stderr}`);
}

const visited = new Set();
let edges = 0;
function walk(file, page) {
  const key = `${page.page} ${file}`;
  if (visited.has(key)) return;
  visited.add(key);
  if (isVendor(file)) return;
  syntax(file);
  const src = stripComments(readFileSync(file, "utf8"));
  for (const { spec, names } of importsOf(src)) {
    const target = resolveSpec(spec, file, page);
    if (!target) continue;
    edges++;
    if (!existsSync(target)) {
      fail(`${rel(file)}: imports '${spec}', and ${rel(target)} does not exist`);
      continue;
    }
    if (names.length && !isVendor(target)) {
      const ex = exportsOf(stripComments(readFileSync(target, "utf8")));
      if (!ex.star) {
        for (const n of names) if (!ex.names.has(n)) fail(`${rel(file)}: imports ${n} from '${spec}', which does not export it`);
      }
    }
    walk(target, page);
  }
}

const hookMap = {};
for (const page of PAGES) {
  const pagePath = join(tools, page.page);
  const html = readFileSync(pagePath, "utf8");
  page.map = importmapOf(html);
  for (const [k, v] of Object.entries(page.map)) {
    if (v.startsWith("/")) fail(`${page.page}: importmap entry '${k}' is root-absolute`);
    let href = pathToFileURL(resolve(dirname(pagePath), v)).href;
    if (k.endsWith("/") && !href.endsWith("/")) href += "/";
    hookMap[k] = href;
  }
  const entries = [...html.matchAll(/<script\b[^>]*\btype="module"[^>]*\bsrc="([^"]+)"/g)].map((m) => m[1]);
  const styles = [...html.matchAll(/<link\b[^>]*\brel="stylesheet"[^>]*\bhref="([^"]+)"/g)].map((m) => m[1]);
  if (!entries.length) fail(`${page.page}: no module entry script found`);
  for (const href of styles) {
    const f = resolveUrl(href, pagePath, page);
    if (f && !existsSync(f)) fail(`${page.page}: stylesheet ${href} does not exist`);
  }
  for (const src of entries) {
    const f = resolveUrl(src, pagePath, page);
    if (!f) continue;
    if (!existsSync(f)) fail(`${page.page}: entry ${src} does not exist`);
    else walk(f, page);
  }
}
const visitedFiles = new Set([...visited].map((k) => rel(k.slice(k.indexOf(" ") + 1))));
for (const f of MUST_VISIT) if (!visitedFiles.has(f)) fail(`${f} was never reached from a page — the walk is not covering what it claims`);

// Really import what loads without a DOM, resolving bare specifiers the way the pages do.
register("data:text/javascript," + encodeURIComponent(`
  let map = {};
  export async function initialize(data) { map = data.map; }
  export async function resolve(spec, ctx, next) {
    let best = null;
    for (const k of Object.keys(map)) if ((k === spec || (k.endsWith("/") && spec.startsWith(k))) && (!best || k.length > best.length)) best = k;
    return next(best ? map[best] + spec.slice(best.length) : spec, ctx);
  }`), { data: { map: hookMap } });

const loaded = {};
for (const f of IMPORTABLE) {
  try {
    loaded[f] = await import(pathToFileURL(join(tools, f)).href);
  } catch (e) {
    fail(`importing ${f} failed: ${e.message}`);
  }
}
const io = loaded["webcommon/io.js"];
if (io) {
  const p = io.studioParams("?studio=1&file=assets%2Fa.map");
  if (!p || p.file !== "assets/a.map") fail(`studioParams read ${JSON.stringify(p)} from ?studio=1&file=assets/a.map`);
  if (io.studioParams("?file=assets/a.map") !== null) fail("studioParams saw studio mode without ?studio=1");
}
const pose = loaded["poser/src/pose.js"];
const lag = loaded["poser/src/lag.gen.js"];
if (pose && lag && pose.LAG !== lag.LAG) fail("pose.js's LAG is not the generated table from lag.gen.js");

for (const p of problems) console.log(`  FAIL ${p}`);
console.log(`studio-modules: ${problems.length ? "FAILED" : "ok"} (${checkedSyntax.size} modules parsed, ${edges} imports resolved, ${Object.keys(loaded).length} imported)`);
process.exit(problems.length ? 1 : 0);
