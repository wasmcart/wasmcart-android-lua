/* parity.mjs — prove the native runtime and the wasm engine agree.
 *
 * Runs the SAME cart, the SAME seed, the SAME fixed 60 Hz step and the SAME
 * scripted input through both:
 *   wasm   — the reference CartHost (wasmcart/src/CartHost.js)
 *   native — build-native/wasmcart-lua-native
 * then compares the resulting frames pixel by pixel.
 *
 * Both sides run an engine built from the SAME sources with the SAME feature
 * set (tools/build-parity-wasm.sh). That matters: comparing against whatever
 * engine a cart happens to be packed with makes every difference ambiguous —
 * engine drift, or port bug?
 *
 *   node tools/parity.mjs [--frames N] [--seed S] [--drive N] [game.wasc ...]
 */
import { CartHost } from '../../wasmcart/src/CartHost.js';
import { execFileSync } from 'child_process';
import { writeFileSync, readFileSync, mkdirSync, existsSync } from 'fs';
import { dirname, basename, resolve } from 'path';
import { fileURLToPath } from 'url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, '..');
const OUT = resolve(ROOT, 'build-parity/shots');
const NATIVE = resolve(ROOT, 'build-native/wasmcart-lua-native');

const args = process.argv.slice(2);
const num = (flag, dflt) => {
  const i = args.indexOf(flag);
  return i >= 0 ? parseInt(args[i + 1], 10) : dflt;
};
const FRAMES = num('--frames', 300);
const SEED   = num('--seed', 12345);
const DRIVE  = num('--drive', 90);
const carts  = args.filter((a) => a.endsWith('.wasc'));
if (!carts.length) { console.error('usage: parity.mjs [opts] game.wasc ...'); process.exit(1); }

const WC_BTN_A = 1 << 0;   // must match wasmcart.h

/* The native harness presses A when (frame % drive) < 3. Replicated EXACTLY
 * here — a parity suite whose two sides receive different input proves
 * nothing, and this is the easiest place for that to silently drift. */
const buttonsForFrame = (frame) =>
  DRIVE && (frame % DRIVE) < 3 ? WC_BTN_A : 0;

function ppmToRGB(path) {
  const buf = readFileSync(path);
  // P6\n<w> <h>\n255\n
  let pos = 0, fields = [];
  while (fields.length < 4) {
    let start = pos;
    while (buf[pos] !== 0x0a && buf[pos] !== 0x20) pos++;
    fields.push(buf.slice(start, pos).toString());
    pos++;
  }
  const w = parseInt(fields[1], 10), h = parseInt(fields[2], 10);
  return { w, h, data: buf.slice(pos) };
}

async function runWasm(cart) {
  const host = new CartHost();
  await host.load(cart, { deterministic: { seed: SEED, stepMs: 1000 / 60 } });
  let frame = null;
  for (let i = 0; i < FRAMES; i++)
    frame = host.runFrame([{ connected: true, buttons: buttonsForFrame(i) }]);

  const gi = host.getInfo();
  const gl = host.getGlContext();
  const w = gi.width, h = gi.height;
  let rgb;
  if (host.usesGL && gl) {
    const rgba = new Uint8Array(w * h * 4);
    gl.finish();
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
    rgb = Buffer.alloc(w * h * 3);
    for (let y = 0; y < h; y++) {              // GL origin is bottom-left
      const src = (h - 1 - y) * w * 4, dst = y * w * 3;
      for (let x = 0; x < w; x++) {
        rgb[dst + x * 3]     = rgba[src + x * 4];
        rgb[dst + x * 3 + 1] = rgba[src + x * 4 + 1];
        rgb[dst + x * 3 + 2] = rgba[src + x * 4 + 2];
      }
    }
  } else {
    const fb = frame.framebuffer;              // XRGB words => B,G,R,X bytes
    rgb = Buffer.alloc(w * h * 3);
    for (let i = 0; i < w * h; i++) {
      rgb[i * 3]     = fb[i * 4 + 2];
      rgb[i * 3 + 1] = fb[i * 4 + 1];
      rgb[i * 3 + 2] = fb[i * 4];
    }
  }
  host.destroy();
  return { w, h, data: rgb };
}

function runNative(cart, shot) {
  execFileSync(NATIVE, [cart, '--frames', String(FRAMES), '--seed', String(SEED),
                        '--drive', String(DRIVE), '--shot', shot, '--headless'],
               { stdio: 'pipe' });
  return ppmToRGB(shot);
}

function compare(a, b) {
  if (a.w !== b.w || a.h !== b.h)
    return { ok: false, reason: `size ${a.w}x${a.h} vs ${b.w}x${b.h}` };
  let diff = 0, maxDelta = 0;
  for (let i = 0; i < a.data.length; i += 3) {
    const d = Math.max(Math.abs(a.data[i] - b.data[i]),
                       Math.abs(a.data[i + 1] - b.data[i + 1]),
                       Math.abs(a.data[i + 2] - b.data[i + 2]));
    if (d > 0) { diff++; if (d > maxDelta) maxDelta = d; }
  }
  const total = a.data.length / 3;
  return { ok: true, diff, total, pct: (diff / total) * 100, maxDelta };
}

mkdirSync(OUT, { recursive: true });
if (!existsSync(NATIVE)) {
  console.error(`no native binary at ${NATIVE} — run tools/build-native-macos.sh`);
  process.exit(1);
}

let failures = 0;
for (const cart of carts) {
  const name = basename(cart, '.wasc');
  process.stdout.write(`${name}: `);
  try {
    const nat = runNative(cart, `${OUT}/${name}-native.ppm`);
    const wasm = await runWasm(cart);
    writeFileSync(`${OUT}/${name}-wasm.ppm`,
      Buffer.concat([Buffer.from(`P6\n${wasm.w} ${wasm.h}\n255\n`), wasm.data]));
    const r = compare(nat, wasm);
    if (!r.ok) { console.log(`FAIL (${r.reason})`); failures++; continue; }
    /* GPU vs GPU is not bit-exact (different rasterizers, different filtering),
     * so the gate is structural: a tiny fraction of pixels may differ slightly.
     * A port bug moves geometry and blows past this instantly. */
    const pass = r.pct < 2.0 && r.maxDelta <= 64;
    console.log(`${pass ? 'PASS' : 'FAIL'} — ${r.pct.toFixed(3)}% pixels differ, max delta ${r.maxDelta} (${nat.w}x${nat.h}, ${FRAMES} frames, seed ${SEED})`);
    if (!pass) failures++;
  } catch (e) {
    console.log(`ERROR ${e.message.split('\n')[0]}`);
    failures++;
  }
}
console.log(failures ? `\n${failures} cart(s) failed parity` : '\nall carts match');
process.exit(failures ? 1 : 0);
