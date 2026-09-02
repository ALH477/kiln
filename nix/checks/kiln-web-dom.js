// SPDX-License-Identifier: MIT
//
// kiln-web-dom.js — the smallest DOM that plat/shell/shell_web.c needs.
//
// The browser launcher's whole job is to get plat/host's finished framebuffer
// onto a canvas and the keyboard off the window. Neither exists under node, so
// a gate that only ran the wasm would prove the wasm loads and nothing about
// the launcher — which is the half that cannot be checked any other way in a
// sandbox with no display and no browser.
//
// So this is a canvas that counts. It is NOT a rendering fake: it records the
// pixels shell_web.c hands it and the check reads them back, so a present hook
// that never fires, a canvas sized wrong, or a page that draws one frame and
// stops all fail here. What it deliberately does not model is compositing,
// scaling and vsync — the three things only a real browser settles, and the
// three things the launcher is not allowed to have an opinion about because
// plat/host has already drawn the frame.
//
// Preloaded with `node --require`, so the globals exist before Emscripten's
// generated JS runs and probes for them.

const stats = { putImageData: 0, width: 0, height: 0, lastNonBlack: 0, lastColours: 0 };
globalThis.__kilnWeb = stats;

function makeContext() {
  return {
    createImageData(w, h) {
      stats.width = w; stats.height = h;
      return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) };
    },
    putImageData(img) {
      stats.putImageData++;
      // Measured here rather than in C so the numbers describe what reached
      // the canvas, not what the framebuffer held a moment earlier.
      let nonBlack = 0;
      const seen = new Set();
      const d = img.data;
      for (let i = 0; i < d.length; i += 4) {
        const p = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        if (p !== 0) nonBlack++;
        if (seen.size < 65536) seen.add(p);
      }
      stats.lastNonBlack = nonBlack;
      stats.lastColours = seen.size;
    },
  };
}

const canvas = {
  id: 'canvas', width: 0, height: 0, style: {},
  getContext: makeContext,
  addEventListener() {},
};

globalThis.document = {
  title: '',
  body: { appendChild() {} },
  getElementById(id) { return id === 'canvas' ? canvas : null; },
  createElement() { return canvas; },
  addEventListener() {},
};

globalThis.window = {
  addEventListener() {},
  // Absent on purpose: no AudioContext and no webkitAudioContext. shell_web.c
  // must treat that as "this machine has no speaker" and keep running, which
  // is the same path a browser tab takes before the user's first gesture. A
  // launcher that hard-required audio would hang here, and hanging before the
  // first frame is exactly the class of bug this gate exists for.
};

globalThis.navigator = globalThis.navigator || { getGamepads: () => [] };
if (!globalThis.navigator.getGamepads) globalThis.navigator.getGamepads = () => [];

process.on('exit', () => {
  console.log(`dom: putImageData ${stats.putImageData} calls, ` +
              `${stats.width}x${stats.height}, ` +
              `last frame non-black ${stats.lastNonBlack}, colours ${stats.lastColours}`);
});
