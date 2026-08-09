#!/usr/bin/env bash
# M1 risk burn-down: build the wasmcart-lua engine NATIVELY on macOS and run
# a cart in an SDL window. Same sources as the wasm build (../wasmcart-lua),
# no emscripten, no V8 — this is the desktop proving ground for the Android
# native runtime. Fast iteration: lldb, instant rebuilds, pixel readback.
set -euo pipefail
cd "$(dirname "$0")/.."
HERE="$(pwd)"

ENGINE="${ENGINE_REPO:-$HERE/../wasmcart-lua}"
WASMCART="${WASMCART_REPO:-$HERE/../wasmcart}"
RT="$ENGINE/runtime"
OUT="$HERE/build-native"
VENDOR="$OUT/vendor"
LUA_VERSION=5.4.7
# upstream has no v3.2.0 tag (max is v3.1.1) — the wasm build.sh pins a
# tag that no longer resolves; physics is optional here either way.
BOX2D_TAG=${BOX2D_TAG:-v3.1.1}
WITH_PHYSICS=${WITH_PHYSICS:-1}

mkdir -p "$OUT" "$VENDOR"

CC=${CC:-clang}
# The engine is C99 + GLES3-shaped GL. On macOS we feed it OpenGL 3.3 core
# through the GL_SILENCE_DEPRECATION path; the GLES3 calls the engine makes
# are a subset that maps 1:1.
COMMON_FLAGS="-O2 -g -fno-omit-frame-pointer -DWC_NATIVE_HOST -DGL_SILENCE_DEPRECATION"

# ── Lua: same version + same patches as the wasm build ──────────────
if [ ! -f "$VENDOR/liblua54.a" ]; then
  if [ ! -d "$VENDOR/lua" ]; then
    echo "== fetching Lua $LUA_VERSION"
    curl -sL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o "$VENDOR/lua.tar.gz"
    tar xzf "$VENDOR/lua.tar.gz" -C "$VENDOR"
    mv "$VENDOR/lua-${LUA_VERSION}" "$VENDOR/lua"
    rm "$VENDOR/lua.tar.gz"
    # the SAME patch set the wasm build applies (idempotent, marker-guarded)
    python3 "$ENGINE/patch-lua.py" "$VENDOR/lua/src/lauxlib.c"
    python3 "$ENGINE/patch-lua.py" "$VENDOR/lua/src/lmathlib.c"
  fi
  echo "== building Lua (native)"
  CORE="lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c \
        lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c \
        ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c lcorolib.c \
        ldblib.c lmathlib.c lstrlib.c ltablib.c lutf8lib.c"
  ( cd "$VENDOR/lua/src" && \
    $CC $COMMON_FLAGS -c $CORE -DLUA_CART_NOFILES \
      -I "$RT" -include "$RT/cartconf.h" && \
    ar rcs "$VENDOR/liblua54.a" ./*.o && rm -f ./*.o )
fi

# ── Box2D v3 (physics.c binds it as the global `b2`) ────────────────
if [ "$WITH_PHYSICS" = "1" ] && [ ! -f "$VENDOR/libbox2d.a" ]; then
  if [ ! -d "$VENDOR/box2d" ]; then
    echo "== fetching Box2D $BOX2D_TAG"
    git clone -q --depth 1 --branch "$BOX2D_TAG" \
      https://github.com/erincatto/box2d.git "$VENDOR/box2d"
    rm -rf "$VENDOR/box2d/.git"
  fi
  echo "== building Box2D (native)"
  ( cd "$VENDOR/box2d/src" && \
    $CC -O2 -c ./*.c -I../include -I. && \
    ar rcs "$VENDOR/libbox2d.a" ./*.o && rm -f ./*.o )
fi

# ── the embedded prelude (same generator as build.sh) ───────────────
python3 - "$RT" <<'PY'
import sys, pathlib
rt = pathlib.Path(sys.argv[1])
data = (rt / 'prelude.lua').read_bytes()
out = rt / 'prelude.inc'
new = ('static const unsigned char WCL_PRELUDE[] = {'
       + ','.join(str(b) for b in data) + '};\n'
       + f'static const unsigned int WCL_PRELUDE_LEN = {len(data)};\n')
if not out.exists() or out.read_text() != new:
    out.write_text(new)
PY

# ── the engine + the desktop harness ────────────────────────────────
echo "== building engine (native)"
# physics is optional: card games never touch it, and Box2D is the single
# biggest chunk of the binary. WITH_PHYSICS=0 swaps in a stub b2 opener.
if [ "$WITH_PHYSICS" = "1" ]; then
  PHYS_SRC="$RT/physics.c"; PHYS_LIB="$VENDOR/libbox2d.a"
  PHYS_INC="-I $VENDOR/box2d/include"
else
  PHYS_SRC="native/physics_stub.c"; PHYS_LIB=""; PHYS_INC=""
fi

$CC $COMMON_FLAGS \
  "$RT/runtime.c" "$RT/vorbis.c" "$RT/cartconf.c" "$PHYS_SRC" \
  "$RT/render2d_gl.c" \
  native/wc_host_native.c native/zip_assets.c native/wc_shell.c \
  tools/desktop_main.c tools/gles_shim.c \
  "$VENDOR/liblua54.a" $PHYS_LIB \
  -DWCL_USE_GL -DWCL_ENABLE_GL2D -DWC_USE_NET_PEER \
  -I "$VENDOR/lua/src" $PHYS_INC -I "$WASMCART/include" \
  -I "$RT" -I native \
  $(sdl2-config --cflags) -I/opt/homebrew/include $(sdl2-config --libs) \
  -framework OpenGL \
  -lz -lm -o "$OUT/wasmcart-lua-native"

echo "built: $OUT/wasmcart-lua-native"
