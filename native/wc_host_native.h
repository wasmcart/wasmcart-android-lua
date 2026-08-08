/* wc_host_native.h — platform-neutral host wiring.
 *
 * The shell (desktop harness, Android activity) fills a wc_host_cfg_t and
 * calls wc_host_native_init() before wc_init(). Everything the engine imports
 * routes through these callbacks, so the same host layer serves macOS/SDL and
 * Android/SDL with no #ifdefs.
 */
#ifndef WC_HOST_NATIVE_H
#define WC_HOST_NATIVE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* log a slice (NOT NUL-terminated) */
    void (*log)(const char *ptr, unsigned int len);

    /* assets: return byte size, or <0 if absent; read fills buf, returns
     * bytes written or <0. Names are pointer+length. */
    void *asset_ctx;
    int (*asset_size)(void *ctx, const char *name, unsigned int name_len);
    int (*asset_read)(void *ctx, const char *name, unsigned int name_len,
                      void *buf, unsigned int buf_len);

    /* pads (state itself is written into the shared region by the shell) */
    int  (*pad_name)(unsigned int pad_id, char *buf, unsigned int buf_len);
    int  (*pad_has_rumble)(unsigned int pad_id);
    void (*pad_rumble)(unsigned int pad_id, float low, float high, unsigned int ms);
    void (*pad_rumble_stop)(unsigned int pad_id);

    /* text input */
    void (*text_begin)(void);
    void (*text_end)(void);
    unsigned int (*text_active)(void);
} wc_host_cfg_t;

void wc_host_native_init(const wc_host_cfg_t *cfg);

/* ── .wasc asset provider (zip_assets.c) ─────────────────────────────
 * A cart is a zip. The engine asks for paths like "main.lua" / "cards/AS.png";
 * inside a .wasc those live under the manifest's `assets` prefix (typically
 * "assets/"). The provider handles that mapping, so the engine is unaware it
 * is reading out of a zip at all.
 */
typedef struct wc_zip wc_zip;

wc_zip *wc_zip_open(const char *path);      /* NULL on failure */
void    wc_zip_close(wc_zip *z);
int     wc_zip_asset_size(void *ctx, const char *name, unsigned int name_len);
int     wc_zip_asset_read(void *ctx, const char *name, unsigned int name_len,
                          void *buf, unsigned int buf_len);
/* manifest.json passthrough (shell reads width/height/name) */
int     wc_zip_read_entry(wc_zip *z, const char *entry, void *buf, unsigned int cap);

#ifdef __cplusplus
}
#endif

#endif /* WC_HOST_NATIVE_H */
