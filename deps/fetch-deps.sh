#!/usr/bin/env bash
# Fetch build dependencies that are not vendored:
#   deps/SDL   — SDL2 source (Java shell + libSDL2.so), pinned tag
#   deps/lua   — Lua 5.4.7, patched exactly as the wasm engine patches it
#
# NOTE what is NOT here: libnode. That ~60 MB prebuilt is the entire reason
# this repo exists. The engine sources come from wasmcart-lua (submodule, or
# $ENGINE_REPO / a sibling checkout).
set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"

SDL_TAG="${SDL_TAG:-release-2.32.10}"
LUA_VERSION="${LUA_VERSION:-5.4.7}"

ENGINE="${ENGINE_REPO:-$HERE/wasmcart-lua}"
[ -d "$ENGINE/runtime" ] || ENGINE="$HERE/../../wasmcart-lua"

if [ ! -d SDL ]; then
    echo "fetching SDL ${SDL_TAG}..."
    git clone --depth 1 --branch "${SDL_TAG}" https://github.com/libsdl-org/SDL.git SDL
fi

# Lua is fetched + patched here rather than vendored, matching
# wasmcart-lua/runtime/build.sh so both targets compile the SAME VM.
if [ ! -d lua ]; then
    echo "fetching Lua ${LUA_VERSION}..."
    curl -sL "https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz" -o lua.tar.gz
    tar xzf lua.tar.gz
    mv "lua-${LUA_VERSION}" lua
    rm lua.tar.gz
    if [ -f "$ENGINE/patch-lua.py" ]; then
        # idempotent, marker-guarded — the same two patches the wasm build applies
        python3 "$ENGINE/patch-lua.py" lua/src/lauxlib.c
        python3 "$ENGINE/patch-lua.py" lua/src/lmathlib.c
    else
        echo "WARNING: no patch-lua.py at $ENGINE — set ENGINE_REPO" >&2
        exit 1
    fi
fi

echo "deps ready:"
echo "  SDL:    $(git -C SDL describe --tags 2>/dev/null || echo '?')"
echo "  Lua:    ${LUA_VERSION} (patched)"
echo "  engine: ${ENGINE}"
