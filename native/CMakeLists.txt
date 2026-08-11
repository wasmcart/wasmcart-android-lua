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
    if(EXISTS "${DEPS}/wasmcart/include/wc_cart.h")
        set(WASMCART_REPO "${DEPS}/wasmcart")          # pinned submodule
    else()
        set(WASMCART_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../wasmcart")
    endif()
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

# ─── Box2D + Box3D (WITH_PHYSICS) ─────────────────────────────────────────
#
# Off by default, matching tools/build-native-macos.sh: no card game touches
# physics, and the two libraries are the largest thing in the binary. A cart
# that DOES use love.physics or b3 needs -DWITH_PHYSICS=ON.
#
# Both are pinned by SHA rather than tag, exactly as the engine's own
# build.sh pins them: no released Box2D has the API physics.c calls, and
# Box3D has no releases at all. The SHAs are read FROM that build.sh so the
# native build cannot silently drift from the wasm one -- a different Box2D
# here would mean the same cart simulating differently on Android, which is
# the kind of divergence nobody would think to look for.
option(WITH_PHYSICS "Build Box2D + Box3D into the engine" OFF)

if(WITH_PHYSICS)
    file(READ "${ENGINE_REPO}/runtime/build.sh" _bs)
    string(REGEX MATCH "BOX2D_SHA=([0-9a-f]+)" _m2 "${_bs}")
    set(BOX2D_SHA "${CMAKE_MATCH_1}")
    string(REGEX MATCH "BOX3D_SHA=([0-9a-f]+)" _m3 "${_bs}")
    set(BOX3D_SHA "${CMAKE_MATCH_1}")
    if(NOT BOX2D_SHA OR NOT BOX3D_SHA)
        message(FATAL_ERROR
            "could not read BOX2D_SHA/BOX3D_SHA out of ${ENGINE_REPO}/runtime/build.sh")
    endif()

    include(FetchContent)
    FetchContent_Declare(box2d_src
        GIT_REPOSITORY https://github.com/erincatto/box2d.git
        GIT_TAG        "${BOX2D_SHA}")
    FetchContent_Declare(box3d_src
        GIT_REPOSITORY https://github.com/erincatto/box3d.git
        GIT_TAG        "${BOX3D_SHA}")
    FetchContent_Populate(box2d_src)
    FetchContent_Populate(box3d_src)

    file(GLOB BOX2D_SRC "${box2d_src_SOURCE_DIR}/src/*.c")
    file(GLOB BOX3D_SRC "${box3d_src_SOURCE_DIR}/src/*.c")
    add_library(box2d STATIC ${BOX2D_SRC})
    target_include_directories(box2d PUBLIC
        "${box2d_src_SOURCE_DIR}/include" PRIVATE "${box2d_src_SOURCE_DIR}/src")
    add_library(box3d STATIC ${BOX3D_SRC})
    target_include_directories(box3d PUBLIC
        "${box3d_src_SOURCE_DIR}/include" PRIVATE "${box3d_src_SOURCE_DIR}/src")

    # Both libraries pick their own SIMD kernels from the CPU macros; this
    # only has to let the compiler emit them. arm64 always has NEON, so
    # nothing extra is needed for the shipping ABI.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        target_compile_options(box2d PRIVATE -mavx2)
        target_compile_options(box3d PRIVATE -mavx2)
        set(PHYS_SIMD "avx2")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(PHYS_SIMD "neon")
    else()
        set(PHYS_SIMD "none")
    endif()
endif()

# ─── libmain.so: engine + native host + SDL entry point ───────────────────
#
# Note what is absent: no libnode, no V8, no gl_imports shim. render2d_gl.c
# and render3d_gl.c call GLES3 directly (wasmcart.h's _GL_IMPORT is a no-op
# off-wasm), so the engine links against the system GLESv3 like any native
# program.

set(ENGINE_SRC
    "${RT}/runtime.c"
    "${RT}/vorbis.c"
    "${RT}/cartconf.c"
    "${RT}/render2d_gl.c"
    # The 3D pipeline: depth buffer, GPU render targets (float/depth/cube/
    # array/volume), MRT, instancing, generic vertex formats. Omitting it
    # would not fail to link -- render2d_gl.c calls into it, so a missing
    # file is a link error -- but it is what makes a 3D cart render at all.
    "${RT}/render3d_gl.c"
)
if(WITH_PHYSICS)
    list(APPEND ENGINE_SRC
        "${RT}/physics.c"       # Box2D, bound as the Lua global `b2`
        "${RT}/physics3d.c"     # Box3D, bound as `b3`
        "${RT}/wc_taskpool.c")  # the shared worker pool both step on
else()
    # runtime.c calls wcl_open_physics()/wcl_open_physics3d() unconditionally,
    # so the symbols must exist even when the libraries do not.
    list(APPEND ENGINE_SRC physics_stub.c)
endif()

add_library(main SHARED
    android_main.c
    wc_host_native.c
    zip_assets.c
    wc_shell.c
    ${ENGINE_SRC}
)

target_compile_definitions(main PRIVATE
    WC_NATIVE_HOST          # real host symbols, real pointers (wc_native.h)
    WCL_USE_GL
    WCL_ENABLE_GL2D         # also gates render3d_gl.c: 3D is GL-only
    WC_USE_NET_PEER         # runtime.c references wc_peer_*; host stubs them
)
if(WITH_PHYSICS)
    # The engine reports this string through b2.simd/b3.simd, so a cart can
    # say which kernels it actually got rather than assume.
    target_compile_definitions(main PRIVATE WC_PHYSICS_SIMD="${PHYS_SIMD}")
endif()

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
if(WITH_PHYSICS)
    target_link_libraries(main PRIVATE box2d box3d)
endif()
