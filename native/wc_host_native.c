/* wc_host_native.c — the wc_* host functions, natively.
 *
 * Under wasm these are `env` imports the embedder supplies. Natively they
 * are ordinary symbols linked beside the engine (see wc_native.h). This file
 * is platform-neutral: everything OS-specific arrives through wc_host_cfg.
 */
#include "wc_host_native.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

static wc_host_cfg_t g_cfg;

void wc_host_native_init(const wc_host_cfg_t *cfg) {
    g_cfg = *cfg;
}

/* ── logging ─────────────────────────────────────────────────────────
 * wc_log takes a POINTER+LENGTH, not a C string: the engine logs slices of
 * Lua strings that are not NUL-terminated. Never hand ptr to printf("%s").
 */
void wc_log(const char *ptr, unsigned int len) {
    if (g_cfg.log) g_cfg.log(ptr, len);
    else fprintf(stderr, "[cart] %.*s\n", (int)len, ptr);
}

void wc_debug_mark(unsigned int id) { (void)id; }

/* Cooperative yield: only meaningful for ASYNCIFY wasm builds that drive
 * their own loop. Native carts return from wc_render every frame. */
void wc_frame_yield(void) {}

/* ── assets ──────────────────────────────────────────────────────────
 * Names are pointer+length (engine-side Lua strings again). The provider
 * owns the storage policy; we only marshal.
 */
int wc_asset_size(const char *name, unsigned int name_len) {
    if (!g_cfg.asset_size) return -1;
    int r = g_cfg.asset_size(g_cfg.asset_ctx, name, name_len);
    if (getenv("WC_TRACE_ASSETS"))
        fprintf(stderr, "[asset] size %.*s -> %d\n", (int)name_len, name, r);
    return r;
}

int wc_load_asset(const char *name, unsigned int name_len,
                  void *buf, unsigned int buf_len) {
    if (!g_cfg.asset_read) return -1;
    int r = g_cfg.asset_read(g_cfg.asset_ctx, name, name_len, buf, buf_len);
    if (getenv("WC_TRACE_ASSETS"))
        fprintf(stderr, "[asset] read %.*s (cap %u) -> %d\n",
                (int)name_len, name, buf_len, r);
    return r;
}

/* ── pads ────────────────────────────────────────────────────────────
 * Pad STATE is written straight into the shared wc_pad_t[4] region by the
 * shell each frame; only the out-of-band queries live here.
 */
int wc_pad_name(unsigned int pad_id, char *buf, unsigned int buf_len) {
    if (!g_cfg.pad_name) return -1;
    return g_cfg.pad_name(pad_id, buf, buf_len);
}

int wc_pad_has_rumble(unsigned int pad_id) {
    return g_cfg.pad_has_rumble ? g_cfg.pad_has_rumble(pad_id) : 0;
}

void wc_pad_rumble(unsigned int pad_id, float low, float high, unsigned int ms) {
    if (g_cfg.pad_rumble) g_cfg.pad_rumble(pad_id, low, high, ms);
}

void wc_pad_rumble_stop(unsigned int pad_id) {
    if (g_cfg.pad_rumble_stop) g_cfg.pad_rumble_stop(pad_id);
}

/* ── text input ──────────────────────────────────────────────────────
 * Card games never type; wired anyway so a cart that asks does not wedge.
 */
void wc_text_input_begin(void) { if (g_cfg.text_begin) g_cfg.text_begin(); }
void wc_text_input_end(void)   { if (g_cfg.text_end)   g_cfg.text_end(); }
unsigned int wc_text_input_active(void) {
    return g_cfg.text_active ? g_cfg.text_active() : 0;
}

/* ── peer networking ─────────────────────────────────────────────────
 * Stubbed: the family games are offline. The engine is built with
 * -DWC_USE_NET_PEER and REFERENCES these, so they must link; each returns
 * the "no transport" answer rather than pretending to connect.
 */
int  wc_peer_open(const char *addr, unsigned int addr_len) {
    (void)addr; (void)addr_len; return -1;
}
void wc_peer_close(int peer_id) { (void)peer_id; }
int  wc_peer_send(int peer_id, const void *data, unsigned int len) {
    (void)peer_id; (void)data; (void)len; return -1;
}
int  wc_peer_broadcast(const void *data, unsigned int len) {
    (void)data; (void)len; return -1;
}
int  wc_peer_count(void) { return 0; }
int  wc_peer_id(unsigned int index) { (void)index; return -1; }
int  wc_peer_state(int peer_id) { (void)peer_id; return 0; }
int  wc_peer_name(int peer_id, char *dest, unsigned int max_len) {
    (void)peer_id; (void)dest; (void)max_len; return -1;
}
int  wc_peer_transport(int peer_id) { (void)peer_id; return 0; }
