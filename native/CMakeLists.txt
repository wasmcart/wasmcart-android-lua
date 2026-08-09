cmake_minimum_required(VERSION 3.22)
project(wasmcart-android-lua C)

set(CMAKE_C_STANDARD 11)

set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../deps")

# Engine sources live in wasmcart-lua and are compiled here, not vendored:
# one home for the engine, two targets (wasm + native).
if(NOT DEFINED ENGINE_REPO)
    if(EXISTS "${DEPS}/wasmcart-lua/runtime/runtime.c")
        set(ENGINE_REPO "${DEPS}/wasmcart-lua")
    else()
        set(ENGINE_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../wasmcart-lua")
    endif()
endif()
set(RT "${ENGINE_REPO}/runtime")

if(NOT DEFINED WASMCART_REPO)
    set(WASMCART_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../wasmcart")
endif()

if(NOT EXISTS "${RT}/runtime.c")
    message(FATAL_ERROR "engine sources not found at ${RT}\n"
        "Set -DENGINE_REPO=/path/to/wasmcart-lua (or add the submodule).")
endif()
if(NOT EXISTS "${DEPS}/lua/src/lua.h")
    message(FATAL_ERROR "Lua not found. Run deps/fetch-deps.sh")
endif()

# ─── SDL2 (fetched source: builds libSDL2.so + supplies the Java shell) ───

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/SDL" "${CMAKE_CURRENT_BINARY_DIR}/SDL2" EXCLUDE_FROM_ALL)

# ─── Lua 5.4.7 ────────────────────────────────────────────────────────────
# Core + the libraries a cart can safely have. NO liolib/loslib/loadlib: the
# cart has no filesystem and no dynamic loading, by design. LUA_CART_NOFILES
# compiles out luaL_loadfilex/luaL_dofile (patch-lua.py). Same set as
# wasmcart-lua/runtime/build.sh — keep them in step.
set(LUA_SRC
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c
    lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c
    ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c lcorolib.c
    ldblib.c lmathlib.c lstrlib.c ltablib.c lutf8lib.c)
list(TRANSFORM LUA_SRC PREPEND "${DEPS}/lua/src/")

add_library(lua54 STATIC ${LUA_SRC})
target_compile_definitions(lua54 PRIVATE LUA_CART_NOFILES)
target_include_directories(lua54 PRIVATE "${RT}")
# cartconf.h must land before lauxlib.h's lua_writestring guards so Lua's
# stdout/stderr hooks resolve to wc_log instead of stdio.
target_compile_options(lua54 PRIVATE -include "${RT}/cartconf.h")

# ─── libmain.so: engine + native host + SDL entry point ───────────────────
#
# Note what is absent: no libnode, no V8, no gl_imports shim. render2d_gl.c
# calls GLES3 directly (wasmcart.h's _GL_IMPORT is a no-op off-wasm), so the
# engine links against the system GLESv3 like any native program.

add_library(main SHARED
    android_main.c
    wc_host_native.c
    zip_assets.c
    physics_stub.c          # see the file: no released Box2D has physics.c's API
    "${RT}/runtime.c"
    "${RT}/vorbis.c"
    "${RT}/cartconf.c"
    "${RT}/render2d_gl.c"
)

target_compile_definitions(main PRIVATE
    WC_NATIVE_HOST          # real host symbols, real pointers (wc_native.h)
    WCL_USE_GL
    WCL_ENABLE_GL2D
    WC_USE_NET_PEER         # runtime.c references wc_peer_*; host stubs them
)

target_include_directories(main PRIVATE
    "${RT}"
    "${DEPS}/lua/src"
    "${WASMCART_REPO}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(main PRIVATE
    SDL2
    lua54
    GLESv3
    z                       # zip_assets.c inflates cart entries
    m log android
)
