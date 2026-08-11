#!/usr/bin/env bash
# Build a wasm engine from the SAME sources and the SAME feature set as the
# native build (physics stubbed), so a parity run compares the port against
# its own source rather than against whatever engine a cart was packed with.
# Without this, differences are ambiguous: engine-version drift or port bug?
set -euo pipefail
cd "$(dirname "$0")/.."
HERE="$(pwd)"
ENGINE="${ENGINE_REPO:-$HERE/deps/wasmcart-lua}"
[ -d "$ENGINE/runtime" ] || ENGINE="$HERE/../wasmcart-lua"
WASMCART="${WASMCART_REPO:-$HERE/deps/wasmcart}"
[ -d "$WASMCART/include" ] || WASMCART="$HERE/../wasmcart"
RT="$ENGINE/runtime"
OUT="$HERE/build-parity"
EMSDK="${EMSDK_ROOT:-$HOME/code/audio/emsdk}"
export PATH="$EMSDK/upstream/emscripten:$PATH"

mkdir -p "$OUT/vendor"

# Lua, same version + patches, compiled for wasm
if [ ! -f "$OUT/vendor/liblua54.a" ]; then
  if [ ! -d "$OUT/vendor/lua" ]; then
    cp -R "$HERE/deps/lua" "$OUT/vendor/lua"    # already fetched + patched
  fi
  CORE="lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c \
        lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c \
        ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c lcorolib.c \
        ldblib.c lmathlib.c lstrlib.c ltablib.c lutf8lib.c"
  ( cd "$OUT/vendor/lua/src" && \
    emcc -O2 -c $CORE -DLUA_CART_NOFILES -sSUPPORT_LONGJMP=wasm \
      -I "$RT" -include "$RT/cartconf.h" && \
    emar rcs "$OUT/vendor/liblua54.a" ./*.o && rm -f ./*.o )
fi

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

# render3d_gl.c must be here for the same reason it is in the native build:
# render2d_gl.c calls into it, and a parity run is only meaningful if BOTH
# sides are built from the same source set. Leaving it off one side would
# compare two different engines and call the difference a port bug.
emcc "$RT/runtime.c" "$RT/vorbis.c" "$RT/cartconf.c" \
  "$RT/render2d_gl.c" "$RT/render3d_gl.c" \
  native/physics_stub.c "$OUT/vendor/liblua54.a" \
  -O2 -msimd128 -msse2 -DWC_USE_NET_PEER -DWCL_USE_GL -DWCL_ENABLE_GL2D \
  -I "$OUT/vendor/lua/src" -I "$WASMCART/include" -I "$RT" \
  -s STANDALONE_WASM=1 --no-entry -sSUPPORT_LONGJMP=wasm \
  -s EXPORTED_FUNCTIONS='["_wc_init","_wc_render","_wc_get_info","_wc_debug_state","_wc_set_seed","_wc_peer_on_connect","_wc_peer_on_message","_wc_peer_on_disconnect","_wc_peer_on_error"]' \
  -s ERROR_ON_UNDEFINED_SYMBOLS=0 \
  -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=67108864 -s STACK_SIZE=4194304 \
  -o "$OUT/engine-parity.wasm"

echo "built $OUT/engine-parity.wasm ($(wc -c < "$OUT/engine-parity.wasm") bytes)"
