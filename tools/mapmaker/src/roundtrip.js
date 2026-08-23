// SPDX-License-Identifier: MIT
// roundtrip.js — headless entry for the nix round-trip check. Reads a .map,
// parses it via mapio.js, re-emits canonical text, writes to the output path.
// Run with: `node tools/mapmaker/src/roundtrip.js <in.map> <out.map>`.

import { readFileSync, writeFileSync } from 'node:fs';
import { parseEditorState, emitMap } from './mapio.js';

const [inPath, outPath] = process.argv.slice(2);
if (!inPath || !outPath) {
  console.error('usage: roundtrip.js <in.map> <out.map>');
  process.exit(2);
}
const text = readFileSync(inPath, 'utf8');
const state = parseEditorState(text);
const out = emitMap(state);
writeFileSync(outPath, out);
console.log(`roundtrip: ${state.brushes.length} brushes, ${state.spawns.length} spawns -> ${outPath}`);