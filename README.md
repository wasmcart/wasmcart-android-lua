# wasmcart-android-lua

A **native** runtime for wasmcart Lua carts. Same `.wasc` files, same engine
sources as [wasmcart-lua](../wasmcart-lua) — compiled with clang/NDK instead
of interpreted as wasm under V8.

Built for sideloaded Android game APKs: the V8-based
[wasmcart-android](../wasmcart-android) player ships ~67 MB per game because
libnode is ~60 MB of it, and a card game never executes a line of JavaScript.
Dropping the wasm layer takes the engine to **0.5 MB stripped**.

This repo does NOT replace wasmcart-android. That stays the universal
any-cart player (mruby, QuickJS, Godot, anything). This is the Lua-only
fast path.

## Status

**Done — shipping on the phone.** All four dad games run on this runtime,
installed over the V8 builds. Measured against those builds, same cart:

| | V8 | native |
|---|---|---|
| APK | 64.2 MB | **6.4 MB** |
| CPU | 60.0% | **36.6%** |
| RAM | 350 MB | **277 MB** |
| frame rate | — | locked 60 fps |
| cold boot | 181 ms | 174 ms (no real change) |

Rendering is **bit-exact** against the wasm engine on all four games — see
Parity below. Plan and milestone history:
[`internal-wasmcart/PLAN.md`](../internal-wasmcart/PLAN.md).

> **Parity is verified on macOS only.** `tools/parity.sh` builds a native
> engine through `build-native-macos.sh`, so the gate cannot run on a Linux
> box. The claim above therefore covers the engine as of the last macOS run,
> not necessarily the current pin — re-run the gate on a Mac after a
> wasmcart-lua bump that touches the renderer.

Re-verified on an API 36 **emulator** after the engine gained 3D and Box3D:
all four games build, install side by side, and play by touch at 55–60 fps
with no fatals. Build for the emulator's own ABI rather than letting an
arm64 APK run under binary translation:

```sh
WC_ABI=x86_64 ./build-game-apk.sh game.wasc
```

## Engine source

The engine is compiled from `wasmcart-lua`, not vendored here. It is found in
this order:

1. `deps/wasmcart-lua` (the pinned submodule), if it contains
   `runtime/render3d_gl.c`
2. a sibling checkout at `../wasmcart-lua`, if it does
3. otherwise the build stops and says the pin is stale

The probe is `render3d_gl.c` rather than `runtime.c` deliberately: it is the
newest source this build needs, so a pin that predates 3D rendering is
rejected as too old instead of being selected and failing later on a missing
file. Override with `cmake -DENGINE_REPO=/path/to/wasmcart-lua`.

### prelude.inc is generated here

`runtime.c` includes `prelude.inc` — the Lua prelude plus the ffi shim,
embedded as a C byte array. In `wasmcart-lua` that file is produced by the
**emscripten** build and is gitignored, so a fresh checkout never contains
it.

CMake therefore generates it at configure time from `prelude.lua` +
`ffi.lua`, in the same order and format, writing into the *build* tree so a
pristine or read-only engine checkout is never modified. The generated
directory goes **before** the engine's `runtime/` on the include path: a
checkout that has also run the wasm build has its own stale copy there, and
compiling that would silently ship a prelude that does not match the
sources.

Before this existed, an Android build against a clean checkout died partway
through the native compile with `fatal error: 'prelude.inc' file not found`.
It only ever worked because a previous wasm build had left a copy lying
around — so it broke for every game at once the moment that was cleaned, and
would have broken CI on any cold cache.

## Try it (macOS)

```sh
git submodule update --init                       # wasmcart-lua + wasmcart, pinned
./deps/fetch-deps.sh                              # SDL2 + Lua (Android build)
./tools/build-native-macos.sh                     # fetches Lua, builds engine
./build-native/wasmcart-lua-native game.wasc      # play it
./build-native/wasmcart-lua-native game.wasc --frames 900 --drive 90 \
    --shot out.ppm                                # headless, scripted, screenshot
```

Flags: `--frames N` (exit after N), `--shot FILE` (PPM screenshot, fixed
60 Hz step for reproducible goldens), `--drive N` (synthesize an A press
every N frames), `--scale S` (shrink the window).

## Build an APK

Any host with the Android SDK + NDK — unlike the desktop harness above, this
path is not macOS-specific.

```sh
./deps/fetch-deps.sh                              # SDL2 + Lua
./build-game-apk.sh game.wasc [icon.png] [out.apk]
```

The cart IS the app: gradle reads the cart's manifest for the launcher label
and derives a unique `applicationId` from it, so every game installs
**alongside** the others rather than over them.

Ships `arm64-v8a`. For an emulator, build its own ABI instead:

```sh
WC_ABI=x86_64 ./build-game-apk.sh game.wasc
adb install -r out/<slug>.apk
```

An arm64 APK *will* run on an x86_64 emulator through libndk_translation,
but under binary translation — so a frame rate measured that way means
nothing, and a crash might belong to the translator rather than the app.

Physics is off by default; `-DWITH_PHYSICS=ON` (see Parity) builds Box2D and
Box3D in.

## Parity

```sh
./tools/parity.sh          # one command, run it at every wasmcart-lua bump
```

Builds a wasm engine and a native engine from the SAME sources with the same
feature set — including `render3d_gl.c`, and physics stubbed on both sides —
packs each game against the wasm one, then drives both through identical
scripted input (same seed, same fixed 60 Hz step) and compares the frames.
Today all four dad games come out **bit-exact: 0.000% of pixels differ, max
delta 0, over 300 frames.**

Both sides must be built from the same source set or the comparison is
meaningless: a file present on one side only would show up as a rendering
difference and be read as a port bug.

Comparing against whatever engine a cart happens to be packed with would make
every difference ambiguous — engine drift, or port bug? Building both sides
from one source removes that question.

`WITH_PHYSICS=1` builds Box2D **and** Box3D in, giving carts the `b2` and
`b3` Lua globals with real worker threads (`wc_taskpool`) and host SIMD —
NEON on arm64, AVX2 on x86_64. Default is off: no card game touches
physics and the two libraries are the largest thing in the binary. See
`native/physics_stub.c`.

The Android build takes the same flag: `-DWITH_PHYSICS=ON`. Both libraries
are pinned by SHA, and the SHAs are **read out of the engine's own
`runtime/build.sh`** rather than repeated here — a different Box2D on
Android would mean the same cart simulating differently on the phone than
in the wasm build, which is the kind of divergence nobody thinks to look
for until a save desyncs.

## 3D

The engine's 3D pipeline (`render3d_gl.c`) is built in unconditionally and
needs nothing extra on Android: it targets GLES3, which is what the phone
has natively. On the desktop harness the same calls go through
`tools/gles_shim.c`; the shim is a GLSL-dialect translation for macOS's
OpenGL 3.3 and is **not** part of the Android build.

That covers the depth buffer, GPU render targets (float / depth /
cube / array / volume formats), multiple render targets, instancing,
colour masking and generic vertex formats — so a 3D cart, including one
built on g3d or 3DreamEngine, renders the same here as under wasm.

## How it works

A `.wasc` is a zip: `manifest.json`, `main.wasm`, and the asset tree. The
wasm hosts execute `main.wasm`; this runtime **ignores it** and serves the
asset tree to the engine it already contains. That is what keeps one
artifact shipping to every platform.

```
game.wasc ──(assets)──> native engine (runtime.c, Lua 5.4.7,
                            │           render2d_gl.c + render3d_gl.c,
                            │           optional Box2D + Box3D)
                            │  direct GLES3 / GL — no marshaling shim
                        native host (wc_host_native.c) ──> SDL2
```

Three things make this cheap rather than a rewrite:

- **`wasmcart.h` was already dual-target.** Every `wc_*` host import is
  `#ifdef __wasm__` with a non-wasm branch, and `_GL_IMPORT` expands to
  nothing off-wasm — so the engine's GL calls become ordinary extern
  declarations that link against real GL. No shim layer exists to have bugs.
- **The engine is plain C99.** Lua compiled clean on the first try; the
  engine needed no source changes beyond the pointer-width seam below.
- **`-sSUPPORT_LONGJMP=wasm` was the reason V8 was required** (Lua's error
  handling is setjmp/longjmp, which needs wasm exception handling). Native
  longjmp just works — the whole constraint disappears.

## The one real ABI seam

`wc_info_t` hands the host every shared region (framebuffer, audio ring,
pads, pointers, save) as a **`uint32_t` offset into wasm linear memory**.
Exact under wasm; lossy on 64-bit native. Same for the debug descriptor.

Upstream (guarded, `WC_NATIVE_HOST`, wasm bytes unchanged):

- `wasmcart-lua/runtime/wc_native.h` — `wc_native_regions()` returns the real
  addresses. The regions are `static` inside the engine, so it hands them out
  once instead of the host reconstructing them from offsets.
- `wasmcart.h` — each host-import block gained an `#elif defined(WC_NATIVE_HOST)`
  branch declaring the imports `extern` (the plain non-wasm branch is
  no-op stubs, which a native host must not get).
- `wasmcart/include/wc_cart.h` — a native debug descriptor holding real
  pointers, because `(uint32_t)(uintptr_t)&x` is neither lossless nor a
  compile-time constant on a 64-bit target.

## macOS-only glue

`tools/gles_shim.c` rewrites the engine's `#version 300 es` shaders to
`#version 330 core` on the way into `glShaderSource`. Android links real
GLES3, where the shaders compile verbatim — this file is not in that build.
