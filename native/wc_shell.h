/* wc_shell.h — audio drain + save persistence, shared by every shell.
 * See wc_shell.c for why these are not in the platform entry points.
 */
#ifndef WC_SHELL_H
#define WC_SHELL_H

#include <SDL.h>
#include "wasmcart.h"
#include "wc_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ~100 ms of 48 kHz stereo f32. Past this the queue is pure latency. */
#define WC_AUDIO_MAX_QUEUED_BYTES (48000 / 10 * 2 * 4)

typedef struct { uint32_t read; uint32_t dropped; } wc_audio_state_t;

SDL_AudioDeviceID wc_audio_open(void);
void wc_audio_reset(wc_audio_state_t *a);
/* returns stereo frames queued this call */
int  wc_audio_drain(wc_audio_state_t *a, SDL_AudioDeviceID dev,
                    const wc_native_regions_t *R);

typedef struct {
    char           path[1024];
    unsigned char *last;
    int            have_last;
} wc_save_state_t;

void wc_save_init(wc_save_state_t *s, const char *dir, const char *cart_path);
int  wc_save_load(wc_save_state_t *s, const wc_native_regions_t *R);   /* bytes read */
int  wc_save_persist(wc_save_state_t *s, const wc_native_regions_t *R); /* 1 = wrote */

#ifdef __cplusplus
}
#endif

#endif /* WC_SHELL_H */
