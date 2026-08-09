/* wc_shell.c — the parts of a host shell that are NOT platform-specific:
 * draining the engine's audio ring into SDL, and persisting the save region.
 *
 * These started in android_main.c. They live here so the desktop harness and
 * the Android activity run the SAME code — otherwise the only way to test
 * saves or audio is to have a device plugged in, which is exactly how a
 * host-layer bug hides until it is in front of a player.
 */
#include "wc_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── audio ───────────────────────────────────────────────────────────
 * The engine mixes interleaved stereo f32 into a ring and advances its own
 * write cursor. The shell owns the read cursor and hands whole runs to SDL.
 */

void wc_audio_reset(wc_audio_state_t *a) { a->read = 0; }

int wc_audio_drain(wc_audio_state_t *a, SDL_AudioDeviceID dev,
                   const wc_native_regions_t *R) {
    if (!dev) return 0;
    uint32_t w = *R->audio_write_cursor, cap = R->audio_cap;
    if (w == a->read) return 0;
    uint32_t avail = (w >= a->read) ? (w - a->read) : (cap - a->read + w);
    if (!avail) return 0;

    /* Never let the device queue grow without bound. If the cart produces
     * faster than the device consumes (a stall, a slow frame), the excess is
     * latency the player hears as everything arriving late — drop it instead.
     */
    uint32_t queued = SDL_GetQueuedAudioSize(dev);
    if (queued > WC_AUDIO_MAX_QUEUED_BYTES) { a->read = w; a->dropped += avail; return 0; }

    int queued_frames = 0;
    for (uint32_t i = 0; i < avail; ) {
        uint32_t idx = (a->read + i) % cap;
        uint32_t run = cap - idx;              /* the ring wraps: up to 2 runs */
        if (run > avail - i) run = avail - i;
        SDL_QueueAudio(dev, R->audio_ring + (size_t)idx * 2,
                       run * 2 * (uint32_t)sizeof(float));
        queued_frames += (int)run;
        i += run;
    }
    a->read = w;
    return queued_frames;
}

SDL_AudioDeviceID wc_audio_open(void) {
    SDL_AudioSpec want, got;
    SDL_zero(want);
    want.freq     = 48000;          /* the ABI's rate; the engine mixes at it */
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = 1024;
    /* Allow the device to pick its own buffer size but NOT its rate/format:
     * a resample or format change under us would silently detune the mix. */
    return SDL_OpenAudioDevice(NULL, 0, &want, &got, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
}

/* ── saves ───────────────────────────────────────────────────────────
 * One blob (the engine's save region), written beside the cart as <cart>.sav.
 */

void wc_save_init(wc_save_state_t *s, const char *dir, const char *cart_path) {
    memset(s, 0, sizeof(*s));
    if (!dir) return;
    const char *base = strrchr(cart_path, '/');
    base = base ? base + 1 : cart_path;
    snprintf(s->path, sizeof(s->path), "%s/%s.sav", dir, base);
}

int wc_save_load(wc_save_state_t *s, const wc_native_regions_t *R) {
    if (!s->path[0]) return 0;
    FILE *f = fopen(s->path, "rb");
    if (!f) return 0;
    size_t n = fread(R->save, 1, R->save_size, f);
    fclose(f);
    /* Seed the dirty-check with what is now in memory, so a load followed by
     * no change does not immediately rewrite the same bytes. */
    if (!s->last) s->last = (unsigned char *)malloc(R->save_size);
    if (s->last) { memcpy(s->last, R->save, R->save_size); s->have_last = 1; }
    return (int)n;
}

int wc_save_persist(wc_save_state_t *s, const wc_native_regions_t *R) {
    if (!s->path[0]) return 0;
    if (s->have_last && memcmp(s->last, R->save, R->save_size) == 0) return 0;
    /* Write to a temp file and rename: a kill mid-write must not leave a
     * truncated save where a good one used to be. */
    char tmp[1088];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t n = fwrite(R->save, 1, R->save_size, f);
    int ok = (fflush(f) == 0);
    fclose(f);
    if (!ok || n != R->save_size) { remove(tmp); return -1; }
    if (rename(tmp, s->path) != 0) { remove(tmp); return -1; }
    if (!s->last) s->last = (unsigned char *)malloc(R->save_size);
    if (s->last) { memcpy(s->last, R->save, R->save_size); s->have_last = 1; }
    return 1;
}
