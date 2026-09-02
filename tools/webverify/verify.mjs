// SPDX-License-Identifier: MIT
//
// tools/webverify/verify.mjs — drive the browser build in a real browser.
//
// nix/checks/kiln-web.nix runs plat/shell/shell_web.c under node against a
// recording DOM stub, which proves the ASYNCIFY loop resumes and the present
// hook reaches a canvas. What it cannot prove is anything about a BROWSER:
// that the page shell works, that a real 2D context accepts the ImageData,
// and — the part with no stub-shaped substitute — that a keypress travels
// from the compositor's event queue through the EM_JS listener into
// kiln_host_pad_set and out the other side as a camera that moved.
//
// So this drives headless Chromium over the DevTools protocol: navigate, let
// the loop run, screenshot, hold a key, screenshot again, and compare. It is
// deliberately NOT a nix check — Chromium is a 150 MB dependency and a
// pre-push gate has no business pulling it — it is `./dev web --verify`, the
// browser-side counterpart to `./dev shot` being a desktop command.
//
// Node's global WebSocket (21+) speaks CDP directly, so there is no puppeteer
// and no node_modules.

const [,, pageUrl, outDir] = process.argv;
if (!pageUrl || !outDir) {
  console.error('usage: verify.mjs <url> <outdir>');
  process.exit(2);
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function targetWs() {
  for (let i = 0; i < 100; i++) {
    try {
      const list = await (await fetch('http://127.0.0.1:9222/json/list')).json();
      const page = list.find(t => t.type === 'page');
      if (page?.webSocketDebuggerUrl) return page.webSocketDebuggerUrl;
    } catch { /* chromium not up yet */ }
    await sleep(100);
  }
  throw new Error('no debuggable page target appeared');
}

const ws = new WebSocket(await targetWs());
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });

let nextId = 1;
const pending = new Map();
const consoleLines = [];
const pageErrors = [];

ws.onmessage = ev => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
  // Everything the page says about itself. A wasm build that half-works is
  // usually loud in the console and silent on screen.
  if (m.method === 'Runtime.consoleAPICalled') {
    consoleLines.push(m.params.type + ': ' +
      m.params.args.map(a => a.value ?? a.description ?? a.type).join(' '));
  }
  if (m.method === 'Runtime.exceptionThrown') {
    pageErrors.push(m.params.exceptionDetails.exception?.description
                 ?? m.params.exceptionDetails.text);
  }
  if (m.method === 'Log.entryAdded' && m.params.entry.level === 'error') {
    pageErrors.push('log: ' + m.params.entry.text);
  }
};

const send = (method, params = {}) => new Promise(res => {
  const id = nextId++;
  pending.set(id, res);
  ws.send(JSON.stringify({ id, method, params }));
});

await send('Runtime.enable');
await send('Log.enable');
await send('Page.enable');

await send('Page.navigate', { url: pageUrl });
await sleep(6000);                       // boot the wasm and run a few seconds

const shot = async name => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  const { writeFileSync } = await import('node:fs');
  writeFileSync(`${outDir}/${name}.png`, Buffer.from(r.result.data, 'base64'));
};

// Two observables, because either alone lies.
//
// The pad mirror (Module.kiln.pad, published by shell_web.c) says what the
// launcher pushed into the engine. That is the input path end to end:
// compositor -> DOM listener -> EM_JS key table -> C -> kiln_host_pad_set.
//
// The pixels say the engine did something with it. Both are needed: the first
// version of this asserted only on the horizontal centre of mass and reported
// "the input path is dead" for a perfectly live one, because engine-demo's
// stick orbits a camera that keeps looking at the origin — the cube stays
// centred and only its angle changes. Lit-pixel count moves; centroid does not.
const pad = async () => (await send('Runtime.evaluate', {
  returnByValue: true,
  expression: `(Module.kiln && Module.kiln.pad) || null`,
})).result.result.value;

const frame = async () => {
  const r = await send('Runtime.evaluate', {
    returnByValue: true,
    expression: `(() => {
      const c = document.getElementById('canvas');
      const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
      let lit = 0, sum = 0;
      for (let y = 60; y < 200; y++) {           // between the two HUD panels
        for (let x = 0; x < c.width; x++) {
          const i = (y * c.width + x) * 4;
          if (d[i] + d[i+1] + d[i+2] > 90) { lit++; sum += x; }
        }
      }
      return { lit, x: lit ? sum / lit : -1, w: c.width, h: c.height };
    })()`,
  });
  return r.result.result.value;
};

const before = await frame();
const padIdle = await pad();
await shot('01-idle');

// Hold D: engine-demo maps the analog stick straight onto the camera X
// (examples/engine/main.c), so a working input path moves the cube left on
// screen. Both keydown and rawKeyDown, because the EM_JS listener is on
// 'keydown' and Chromium only synthesises that from a char-bearing event.
for (const type of ['rawKeyDown', 'keyDown']) {
  await send('Input.dispatchKeyEvent', {
    type, key: 'd', code: 'KeyD', windowsVirtualKeyCode: 68, nativeVirtualKeyCode: 68,
    text: 'd', unmodifiedText: 'd',
  });
}
await sleep(2000);
const held = await frame();
const padHeld = await pad();
await shot('02-key-held');

await send('Input.dispatchKeyEvent', {
  type: 'keyUp', key: 'd', code: 'KeyD',
  windowsVirtualKeyCode: 68, nativeVirtualKeyCode: 68,
});
await sleep(2000);
const released = await frame();
const padReleased = await pad();
await shot('03-key-released');

const report = { before, held, released, padIdle, padHeld, padReleased,
                 consoleLines, pageErrors };
const { writeFileSync } = await import('node:fs');
writeFileSync(`${outDir}/report.json`, JSON.stringify(report, null, 2));

console.log('canvas            : %dx%d', before.w, before.h);
console.log('pad frames pushed : %d', padReleased ? padReleased.frame : -1);
console.log('pad stick_x       : idle %s, D held %s, released %s',
            padIdle?.stick_x, padHeld?.stick_x, padReleased?.stick_x);
console.log('lit pixels        : idle %d, held %d, released %d',
            before.lit, held.lit, released.lit);
console.log('console lines     : %d', consoleLines.length);
for (const l of consoleLines.slice(0, 10)) console.log('  ' + l);
console.log('page errors       : %d', pageErrors.length);
for (const e of pageErrors.slice(0, 10)) console.log('  ' + e);

const bad = [];
if (before.w !== 320 || before.h !== 240) bad.push(`canvas is ${before.w}x${before.h}, not 320x240`);
if (before.lit < 500)                     bad.push(`only ${before.lit} lit pixels: nothing rendered`);
if (pageErrors.length)                    bad.push(`${pageErrors.length} page errors`);
if (!padIdle)                             bad.push('the launcher never published a pad: vsync is not running');
if (padIdle && padIdle.frame < 60)        bad.push(`only ${padIdle.frame} frames in 6 s: the ASYNCIFY loop is stalling`);
if (padIdle?.stick_x !== 0)               bad.push(`stick_x is ${padIdle?.stick_x} with nothing held`);
// JOYPAD_RANGE_N64_STICK_MAX. Anything less means the key map or the stick
// shaping changed, which is worth failing on rather than rounding off.
if (padHeld?.stick_x !== 90)              bad.push(`holding D gave stick_x ${padHeld?.stick_x}, expected 90`);
if (padReleased?.stick_x !== 0)           bad.push(`stick_x stuck at ${padReleased?.stick_x} after keyup`);
// And the engine did something with it. engine-demo orbits the camera, so
// the count of lit pixels moves even though the cube stays centred.
if (Math.abs(held.lit - before.lit) < 100) bad.push(
  `the frame barely changed while D was held (${before.lit} -> ${held.lit} lit): ` +
  `the pad reached the launcher but not the game`);

if (bad.length) { console.error('\nFAILED: ' + bad.join('; ')); process.exit(1); }
console.log('\nthe browser build renders, loops, and the keyboard reaches the game');
ws.close();
