/* android_main.c — SDL entry point for the NATIVE Lua runtime.
 *
 * Adapted from wasmcart-android's, with the V8 host removed: there is no
 * wc_host_* object, no wasm memory, no GL import shim. The engine is linked
 * in, so this file talks to it directly (wc_get_info / wc_init / wc_render)
 * and writes input into the shared regions wc_native_regions() hands over.
 *
 * argv[1] = cart path, argv[2] = saves dir (WasmcartActivity's contract).
 */
#include <SDL.h>
#include <GLES3/gl3.h>
#include <android/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wc_host_native.h"
#include "wasmcart.h"
#include "wc_native.h"

extern wc_info_t *wc_get_info(void);
extern void wc_init(void);
extern void wc_render(void);
extern void wc_set_seed(unsigned int seed);
extern int  wcl_r2d_active(void);

#define TAG "wasmcart"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define STEP_MS      (1000.0 / 60.0)
#define MAX_STEPS    4
#define SAVE_EVERY   600            /* frames between save checks (~10 s) */

static wc_zip *g_zip;
static char    g_save_path[1024];

/* ── host callbacks ─────────────────────────────────────────────────── */

static void host_log(const char *ptr, unsigned int len) {
    /* pointer+length, never NUL-terminated — %.*s, not %s */
    __android_log_print(ANDROID_LOG_INFO, TAG, "[cart] %.*s", (int)len, ptr);
}

static int pad_name_cb(unsigned int pad_id, char *buf, unsigned int cap) {
    SDL_GameController *c = SDL_GameControllerFromPlayerIndex((int)pad_id);
    const char *n = c ? SDL_GameControllerName(c) : NULL;
    if (!n) return -1;
    unsigned l = (unsigned)strlen(n);
    if (l >= cap) l = cap - 1;
    memcpy(buf, n, l);
    buf[l] = 0;
    return (int)l;
}

static int pad_has_rumble_cb(unsigned int pad_id) {
    SDL_GameController *c = SDL_GameControllerFromPlayerIndex((int)pad_id);
    return c && SDL_GameControllerHasRumble(c);
}
static void pad_rumble_cb(unsigned int pad_id, float low, float high, unsigned int ms) {
    SDL_GameController *c = SDL_GameControllerFromPlayerIndex((int)pad_id);
    if (c) SDL_GameControllerRumble(c, (Uint16)(low * 65535.0f),
                                    (Uint16)(high * 65535.0f), ms);
}
static void pad_rumble_stop_cb(unsigned int pad_id) {
    SDL_GameController *c = SDL_GameControllerFromPlayerIndex((int)pad_id);
    if (c) SDL_GameControllerRumble(c, 0, 0, 0);
}

/* ── saves ──────────────────────────────────────────────────────────── */

static void save_path_init(const char *dir, const char *cart) {
    const char *base = strrchr(cart, '/');
    base = base ? base + 1 : cart;
    snprintf(g_save_path, sizeof(g_save_path), "%s/%s.sav", dir, base);
}

static void save_load(const wc_native_regions_t *R) {
    if (!g_save_path[0]) return;
    FILE *f = fopen(g_save_path, "rb");
    if (!f) return;
    size_t n = fread(R->save, 1, R->save_size, f);
    fclose(f);
    LOGI("save: restored %zu bytes", n);
}

static void save_persist(const wc_native_regions_t *R) {
    static unsigned char *last;
    static int have_last;
    if (!g_save_path[0]) return;
    if (have_last && memcmp(last, R->save, R->save_size) == 0) return;  /* unchanged */
    FILE *f = fopen(g_save_path, "wb");
    if (!f) return;
    fwrite(R->save, 1, R->save_size, f);
    fclose(f);
    if (!last) last = (unsigned char *)malloc(R->save_size);
    if (last) { memcpy(last, R->save, R->save_size); have_last = 1; }
}

/* ── letterbox ──────────────────────────────────────────────────────── */

typedef struct { int x, y, w, h; } rect_t;

static rect_t fit_rect(int sw, int sh, int dw, int dh) {
    rect_t r;
    float sa = (float)sw / (float)sh, da = (float)dw / (float)dh;
    if (da > sa) { r.h = dh; r.w = (int)(dh * sa); }
    else         { r.w = dw; r.h = (int)(dw / sa); }
    r.x = (dw - r.w) / 2;
    r.y = (dh - r.h) / 2;
    return r;
}

/* ── audio ──────────────────────────────────────────────────────────── */

static SDL_AudioDeviceID g_audio;
static uint32_t g_audio_read;

static void audio_drain(const wc_native_regions_t *R) {
    if (!g_audio) return;
    uint32_t w = *R->audio_write_cursor, cap = R->audio_cap;
    if (w == g_audio_read) return;
    uint32_t avail = (w >= g_audio_read) ? (w - g_audio_read)
                                         : (cap - g_audio_read + w);
    if (!avail) return;
    /* interleaved stereo f32; the ring wraps, so queue it in up to two runs */
    for (uint32_t i = 0; i < avail; ) {
        uint32_t idx = (g_audio_read + i) % cap;
        uint32_t run = cap - idx;
        if (run > avail - i) run = avail - i;
        SDL_QueueAudio(g_audio, R->audio_ring + (size_t)idx * 2,
                       run * 2 * sizeof(float));
        i += run;
    }
    g_audio_read = w;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) { LOGE("no cart path in argv"); return 1; }
    const char *cart_path = argv[1];
    const char *saves_dir = argc > 2 ? argv[2] : NULL;

    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");  /* no ghost mouse from touch */
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");  /* no ghost touch from mouse */
    /* SDLActivity.setOrientation overrides the manifest at window creation;
     * this hint is what it actually reads. Both landscapes so the device can
     * be held either way. (Same trap as the V8 player.) */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        LOGE("SDL_Init: %s", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window *win = SDL_CreateWindow("wasmcart",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE);
    if (!win) { LOGE("SDL_CreateWindow: %s", SDL_GetError()); return 1; }
    SDL_GLContext glc = SDL_GL_CreateContext(win);
    if (!glc) { LOGE("SDL_GL_CreateContext: %s", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);

    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(win, &win_w, &win_h);
    LOGI("display %dx%d, GL: %s", win_w, win_h, (const char *)glGetString(GL_VERSION));

    /* ── the cart ── */
    g_zip = wc_zip_open(cart_path);
    if (!g_zip) { LOGE("cannot open cart: %s", cart_path); return 1; }

    wc_host_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.log             = host_log;
    cfg.asset_ctx       = g_zip;
    cfg.asset_size      = wc_zip_asset_size;
    cfg.asset_read      = wc_zip_asset_read;
    cfg.pad_name        = pad_name_cb;
    cfg.pad_has_rumble  = pad_has_rumble_cb;
    cfg.pad_rumble      = pad_rumble_cb;
    cfg.pad_rumble_stop = pad_rumble_stop_cb;
    wc_host_native_init(&cfg);

    wc_info_t *info = wc_get_info();
    const wc_native_regions_t *R = wc_native_regions();

    /* prior save must be in memory before wc_init reads it */
    if (saves_dir) { save_path_init(saves_dir, cart_path); save_load(R); }

    R->host_info->preferred_width   = (uint32_t)win_w;
    R->host_info->preferred_height  = (uint32_t)win_h;
    R->host_info->audio_sample_rate = 48000;

    wc_set_seed((unsigned)SDL_GetPerformanceCounter());
    wc_init();

    int cw = (int)info->width, ch = (int)info->height;
    LOGI("cart %dx%d", cw, ch);

    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else LOGE("audio: %s", SDL_GetError());

    /* The engine renders straight to the default framebuffer in CART
     * coordinates, so the letterbox is just the viewport — no redirect FBO,
     * which is precisely the machinery that produced three host bugs in the
     * V8 player. Caveat: wcl_r2d_target(NULL) resets the viewport to
     * (0,0,cart_w,cart_h), so a cart that uses canvases mid-frame would need
     * the engine to restore a host rect instead. No card game does. */
    rect_t vp = fit_rect(cw, ch, win_w, win_h);

    int running = 1, suspended = 0;
    uint32_t frame = 0;
    double t_ms = 0.0, acc = 0.0;
    Uint64 prev = SDL_GetPerformanceCounter();
    const Uint64 freq = SDL_GetPerformanceFrequency();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_APP_WILLENTERBACKGROUND: suspended = 1; save_persist(R); break;
            case SDL_APP_DIDENTERFOREGROUND:  suspended = 0; prev = SDL_GetPerformanceCounter(); break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GL_GetDrawableSize(win, &win_w, &win_h);
                    vp = fit_rect(cw, ch, win_w, win_h);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP: {
                /* Touch → pointer slots 1-9 (slot 0 is reserved for a mouse
                 * by the ABI). Normalized device coords come back 0..1 over
                 * the WINDOW; map through the letterbox into cart space, or
                 * every tap lands offset on a non-16:9 screen. */
                int slot = 1 + (int)(ev.tfinger.fingerId % 9);
                float px = ev.tfinger.x * (float)win_w;
                float py = ev.tfinger.y * (float)win_h;
                int cx = (int)((px - vp.x) * (float)cw / (float)vp.w);
                int cy = (int)((py - vp.y) * (float)ch / (float)vp.h);
                wc_pointer_t *p = &R->pointers[slot];
                p->x = (int16_t)cx;
                p->y = (int16_t)cy;
                if (ev.type == SDL_FINGERUP) { p->buttons = 0; p->active = 0; }
                else                          { p->buttons = 1; p->active = 1; }
                break;
            }
            default: break;
            }
        }

        if (suspended) { SDL_Delay(50); continue; }

        Uint64 now = SDL_GetPerformanceCounter();
        double real = (double)(now - prev) * 1000.0 / (double)freq;
        prev = now;
        if (real > 250.0) real = 250.0;      /* the ABI's stall clamp */
        acc += real;

        /* pads: SDL controllers → the shared region, no marshaling */
        memset(R->pads, 0, sizeof(wc_pad_t) * 4);
        for (int i = 0; i < 4; i++) {
            SDL_GameController *c = SDL_GameControllerFromPlayerIndex(i);
            if (!c) continue;
            wc_pad_t *pad = &R->pads[i];
            pad->connected = 1;
            struct { SDL_GameControllerButton b; uint16_t m; } map[] = {
                { SDL_CONTROLLER_BUTTON_A, WC_BTN_A },
                { SDL_CONTROLLER_BUTTON_B, WC_BTN_B },
                { SDL_CONTROLLER_BUTTON_X, WC_BTN_X },
                { SDL_CONTROLLER_BUTTON_Y, WC_BTN_Y },
                { SDL_CONTROLLER_BUTTON_START, WC_BTN_START },
                { SDL_CONTROLLER_BUTTON_BACK, WC_BTN_SELECT },
                { SDL_CONTROLLER_BUTTON_DPAD_UP, WC_BTN_UP },
                { SDL_CONTROLLER_BUTTON_DPAD_DOWN, WC_BTN_DOWN },
                { SDL_CONTROLLER_BUTTON_DPAD_LEFT, WC_BTN_LEFT },
                { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, WC_BTN_RIGHT },
                { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, WC_BTN_L },
                { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, WC_BTN_R },
            };
            for (size_t k = 0; k < sizeof(map)/sizeof(map[0]); k++)
                if (SDL_GameControllerGetButton(c, map[k].b)) pad->buttons |= map[k].m;
        }

        int steps = 0;
        while (acc >= STEP_MS && steps < MAX_STEPS) {
            t_ms += STEP_MS;
            R->time->time_ms  = t_ms;
            R->time->delta_ms = STEP_MS;
            R->time->frame    = frame;

            glViewport(vp.x, vp.y, vp.w, vp.h);
            wc_render();
            audio_drain(R);

            frame++; steps++; acc -= STEP_MS;
        }
        if (steps == MAX_STEPS) acc = 0.0;   /* dropped time, don't spiral */

        if (steps > 0) {
            SDL_GL_SwapWindow(win);
            /* Frame rate, once a second. Cheap, and the only honest way to
             * compare this runtime against the V8 player rather than trading
             * impressions about which "feels" smoother. */
            static Uint64 fps_t0; static uint32_t fps_f0;
            if (!fps_t0) { fps_t0 = now; fps_f0 = frame; }
            else if (now - fps_t0 >= freq) {
                LOGI("fps %.1f", (double)(frame - fps_f0) * (double)freq / (double)(now - fps_t0));
                fps_t0 = now; fps_f0 = frame;
            }
        }
        else SDL_Delay(1);

        if (frame % SAVE_EVERY == 0) save_persist(R);
    }

    save_persist(R);
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    wc_zip_close(g_zip);
    SDL_GL_DeleteContext(glc);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
