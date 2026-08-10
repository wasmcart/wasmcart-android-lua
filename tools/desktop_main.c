/* desktop_main.c — macOS/SDL harness for the NATIVE wasmcart-lua engine.
 *
 * M1 proving ground: same engine sources as the wasm build, compiled with
 * clang, driving a real GL context. Exists so the Android port can be
 * debugged with lldb and pixel-compared against the wasm goldens before any
 * of it has to survive a device round-trip.
 *
 *   wasmcart-lua-native game.wasc [--frames N --shot out.ppm] [--scale S]
 */
#include <SDL2/SDL.h>
/* Core-profile entry points: SDL_opengl.h on macOS only exposes the 2.1
 * surface. gl3.h is the core-profile header the harness needs. */
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc_host_native.h"
/* wasmcart.h only declares the GL surface (and its GLenum/GLuint aliases)
 * under WC_USE_GL, exactly as runtime.c does. SDL_opengl.h above already
 * pulled in the real GL types, so keep them from colliding: we need the
 * wc_* structs here, not the GL block. */
#include "wasmcart.h"
#include "wc_native.h"
#include "wc_shell.h"

/* engine entry points (runtime.c) */
extern wc_info_t *wc_get_info(void);
extern void wc_init(void);
extern void wc_render(void);
extern void wc_set_seed(unsigned int seed);

static wc_zip *g_zip;

static void host_log(const char *ptr, unsigned int len) {
    fprintf(stderr, "[cart] %.*s\n", (int)len, ptr);
}

static int pad_name(unsigned int id, char *buf, unsigned int cap) {
    const char *n = (id == 0) ? "Keyboard" : "";
    unsigned l = (unsigned)strlen(n);
    if (l >= cap) return -1;
    memcpy(buf, n, l + 1);
    return (int)l;
}

/* Keyboard → pad 0, matching the reference hosts' mapping so a cart plays
 * the same here as under wasmcart-play. */
static uint16_t keys_to_buttons(const Uint8 *k) {
    uint16_t b = 0;
    if (k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]) b |= WC_BTN_LEFT;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) b |= WC_BTN_RIGHT;
    if (k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]) b |= WC_BTN_UP;
    if (k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]) b |= WC_BTN_DOWN;
    if (k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_SPACE]) b |= WC_BTN_A;
    if (k[SDL_SCANCODE_X])                          b |= WC_BTN_B;
    if (k[SDL_SCANCODE_C])                          b |= WC_BTN_X;
    if (k[SDL_SCANCODE_V])                          b |= WC_BTN_Y;
    if (k[SDL_SCANCODE_RETURN])                     b |= WC_BTN_START;
    if (k[SDL_SCANCODE_TAB] || k[SDL_SCANCODE_BACKSPACE]) b |= WC_BTN_SELECT;
    return b;
}

/* ── CPU-path presentation ───────────────────────────────────────────
 * The engine presents GL frames itself. When it falls back to the software
 * rasterizer it only fills wc_framebuffer, so the shell uploads that as a
 * texture and draws a fullscreen triangle — the same job blit_2d_frame does
 * in the reference Android host.
 */
extern int wcl_r2d_active(void);

static GLuint cpu_tex, cpu_prog, cpu_vao;
static int cpu_tw, cpu_th;

static GLuint mk_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

static void present_cpu_if_needed(const uint32_t *fb, int w, int h) {
    if (wcl_r2d_active()) return;          /* GL path already presented */
    if (!cpu_prog) {
        const char *vs =
            "#version 330 core\n"
            "out vec2 uv;\n"
            "void main(){float x=float((gl_VertexID&1)<<2)-1.0;"
            "float y=float((gl_VertexID&2)<<1)-1.0;"
            "uv=vec2((x+1.0)*0.5,1.0-(y+1.0)*0.5);"
            "gl_Position=vec4(x,y,0,1);}\n";
        const char *fs =
            "#version 330 core\n"
            "in vec2 uv; out vec4 c; uniform sampler2D t;\n"
            /* engine framebuffer words are XRGB little-endian => BGRA bytes */
            "void main(){c=vec4(texture(t,uv).bgr,1.0);}\n";
        cpu_prog = glCreateProgram();
        glAttachShader(cpu_prog, mk_shader(GL_VERTEX_SHADER, vs));
        glAttachShader(cpu_prog, mk_shader(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(cpu_prog);
        glGenVertexArrays(1, &cpu_vao);
        glGenTextures(1, &cpu_tex);
        glBindTexture(GL_TEXTURE_2D, cpu_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    int dw, dh;
    SDL_GL_GetDrawableSize(SDL_GL_GetCurrentWindow(), &dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cpu_tex);
    if (w != cpu_tw || h != cpu_th) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fb);
        cpu_tw = w; cpu_th = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    }
    glUseProgram(cpu_prog);
    glBindVertexArray(cpu_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void write_ppm(const char *path, const unsigned char *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    /* GL readback is bottom-left origin; flip to top-left for the file */
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++)
            fwrite(rgba + ((size_t)y * w + x) * 4, 1, 3, f);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *cart = NULL, *shot = NULL, *saves = NULL;
    int want_frames = 0, scale = 1, drive = 0, headless = 0;
    unsigned seed = 0; int have_seed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) want_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--drive") && i + 1 < argc) drive = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc) saves = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { seed = (unsigned)strtoul(argv[++i], NULL, 10); have_seed = 1; }
        else if (!strcmp(argv[i], "--headless")) headless = 1;
        else if (argv[i][0] != '-') cart = argv[i];
    }
    if (!cart) { fprintf(stderr, "usage: %s game.wasc [--frames N --shot out.ppm]\n", argv[0]); return 1; }

    g_zip = wc_zip_open(cart);
    if (!g_zip) { fprintf(stderr, "cannot open cart: %s\n", cart); return 1; }

    wc_host_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log         = host_log;
    cfg.asset_ctx   = g_zip;
    cfg.asset_size  = wc_zip_asset_size;
    cfg.asset_read  = wc_zip_asset_read;
    cfg.pad_name    = pad_name;
    wc_host_native_init(&cfg);

    /* info BEFORE init: default resolution + region addresses */
    wc_info_t *info = wc_get_info();
    const wc_native_regions_t *R = wc_native_regions();
    int cw = (int)info->width, ch = (int)info->height;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    /* The engine's GLSL is "#version 300 es". macOS gives that surface as
     * GL 3.3 core (the shaders' precision qualifiers are accepted); Android
     * gets real GLES3, where this is native. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *win = SDL_CreateWindow("wasmcart-lua (native)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        cw / scale, ch / scale, SDL_WINDOW_OPENGL |
        /* HiDPI gives a 2x drawable on Retina, which is lovely to look at and
         * useless for parity: the wasm side renders at cart size. Headless
         * runs stay 1:1 so frames are directly comparable. */
        /* NOT SDL_WINDOW_HIDDEN: on macOS a hidden window's GL surface is
         * not reliably sized, and the readback comes back offset. A visible
         * 1:1 window is the price of a trustworthy comparison. */
        (headless ? 0 : SDL_WINDOW_ALLOW_HIGHDPI));
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(shot ? 0 : 1);
    fprintf(stderr, "GL: %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    /* host_info before wc_init: the cart's conf.lua may adapt to it */
    R->host_info->preferred_width  = (uint32_t)cw;
    R->host_info->preferred_height = (uint32_t)ch;
    R->host_info->audio_sample_rate = 48000;

    /* prior save must be in memory before wc_init reads it */
    static wc_save_state_t save_st;
    if (saves) {
        wc_save_init(&save_st, saves, cart);
        int n = wc_save_load(&save_st, R);
        if (n > 0) fprintf(stderr, "save: restored %d bytes\n", n);
    }

    static wc_audio_state_t audio_st;
    SDL_AudioDeviceID audio = wc_audio_open();
    if (audio) { wc_audio_reset(&audio_st); SDL_PauseAudioDevice(audio, 0); }
    else fprintf(stderr, "audio: %s\n", SDL_GetError());

    wc_set_seed(have_seed ? seed : (unsigned)SDL_GetPerformanceCounter());
    wc_init();

    /* conf.lua may have resized the cart — re-read and resize the window */
    if ((int)info->width != cw || (int)info->height != ch) {
        cw = (int)info->width; ch = (int)info->height;
        SDL_SetWindowSize(win, cw / scale, ch / scale);
    }
    fprintf(stderr, "cart: %dx%d\n", cw, ch);

    /* Headless parity runs must not depend on the window the display is
     * willing to give us: a 1920x1080 request on a smaller screen gets
     * clamped, and the readback then holds a smaller viewport with the rest
     * of the frame stale. Render into a cart-sized FBO instead.
     * (Safe for these carts: the engine only re-binds FBO 0 when returning
     * from a canvas, which none of them use.) */
    GLuint hl_fbo = 0, hl_tex = 0;
    if (headless) {
        /* The engine CACHES its GL bindings (render2d_gl.c keeps
         * `bound_texture` and skips a redundant glBindTexture). Creating our
         * own texture here silently invalidates that cache: the engine then
         * believes the atlas is bound while OUR texture actually is, and its
         * next glTexSubImage2D uploads the sprite into our FBO texture. The
         * atlas stays empty and sprites never appear -- with no GL error.
         *
         * So: capture what is bound now (the engine's atlas), do our work,
         * and put it back. Same law the Android host learned the hard way --
         * a shell must leave every piece of GL state exactly as it found it. */
        GLint engine_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &engine_tex);

        glGenTextures(1, &hl_tex);
        glBindTexture(GL_TEXTURE_2D, hl_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &hl_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, hl_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hl_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "headless FBO incomplete\n");
        glViewport(0, 0, cw, ch);

        glBindTexture(GL_TEXTURE_2D, (GLuint)engine_tex);   /* restore */
    }

    int running = 1, frame = 0;
    long total_audio = 0;
    double t_ms = 0.0;
    const double STEP_MS = 1000.0 / 60.0;
    Uint64 prev = SDL_GetPerformanceCounter();
    const Uint64 freq = SDL_GetPerformanceFrequency();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }

        /* pads + pointer written STRAIGHT into the engine's regions — the
         * whole marshaling layer the wasm host needs simply does not exist */
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        memset(R->pads, 0, sizeof(wc_pad_t) * 4);
        R->pads[0].connected = 1;
        R->pads[0].buttons = keys_to_buttons(keys);
        /* --drive N: synthesize an A press every N frames. Headless proof
         * that input reaches the cart and the game logic advances. */
        if (drive && (frame % drive) < 3) R->pads[0].buttons |= WC_BTN_A;

        int mx, my;
        Uint32 mb = SDL_GetMouseState(&mx, &my);
        R->pointers[0].x = (int16_t)(mx * scale);
        R->pointers[0].y = (int16_t)(my * scale);
        R->pointers[0].buttons = (uint8_t)((mb & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1 : 0);
        R->pointers[0].active = 1;

        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - prev) * 1000.0 / (double)freq;
        prev = now;
        if (shot || have_seed) dt = STEP_MS;  /* deterministic for goldens */
        if (dt > 250.0) dt = 250.0;           /* the ABI's stall clamp */
        t_ms += dt;
        R->time->time_ms  = t_ms;
        R->time->delta_ms = dt;
        R->time->frame    = (uint32_t)frame;

        if (headless) { glBindFramebuffer(GL_FRAMEBUFFER, hl_fbo); glViewport(0, 0, cw, ch); }
        wc_render();
        total_audio += wc_audio_drain(&audio_st, audio, R);
        /* If the engine fell back to its software rasterizer it has written
         * wc_framebuffer and presented nothing (wc_gl_blit is suppressed when
         * GL never came up). Present it ourselves so the CPU path is visible
         * and screenshot-able — the reference hosts do the same. */
        present_cpu_if_needed(R->framebuffer, cw, ch);
        SDL_GL_SwapWindow(win);
        frame++;

        if (want_frames && frame >= want_frames) {
            if (shot) {
                int pw, ph;
                if (headless) { pw = cw; ph = ch; glBindFramebuffer(GL_FRAMEBUFFER, hl_fbo); }
                else SDL_GL_GetDrawableSize(win, &pw, &ph);
                unsigned char *px = (unsigned char *)malloc((size_t)pw * ph * 4);
                glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px);
                write_ppm(shot, px, pw, ph);
                free(px);
                fprintf(stderr, "shot: %s (%dx%d)\n", shot, pw, ph);
            }
            running = 0;
        }
    }

    if (saves) {
        int wrote = wc_save_persist(&save_st, R);
        fprintf(stderr, "save: %s (%s)\n", wrote > 0 ? "written" : "unchanged", save_st.path);
    }
    fprintf(stderr, "audio: %ld frames queued (%.2f s), %u dropped (%.2f s)\n",
            total_audio, (double)total_audio / 48000.0,
            audio_st.dropped, (double)audio_st.dropped / 48000.0);
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    wc_zip_close(g_zip);
    return 0;
}
