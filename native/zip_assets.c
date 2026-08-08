/* zip_assets.c — read cart assets straight out of the .wasc.
 *
 * A .wasc IS a zip (manifest.json + main.wasm + the asset tree). The native
 * runtime ignores main.wasm entirely — it IS the engine — and serves the
 * asset tree to the engine through this reader. That is what lets one
 * artifact ship to every platform: the wasm hosts execute main.wasm, this
 * host skips it.
 *
 * Deliberately minimal: central-directory parse, stored + deflate. No
 * zip64, no encryption, no streaming — carts are tens of MB at most and the
 * whole file is mmap-able. zlib comes with both macOS and the NDK.
 */
#include "wc_host_native.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    char    *name;
    uint32_t comp_size, uncomp_size, local_off;
    uint16_t method;
} zentry;

struct wc_zip {
    FILE    *f;
    zentry  *entries;
    int      count;
    char    *prefix;      /* manifest `assets` value, e.g. "assets/" */
    unsigned prefix_len;
};

#include <stdint.h>

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int find_eocd(FILE *f, long *eocd_off, long file_size) {
    /* EOCD is at the end, but a zip comment can push it back up to 64K. */
    long span = file_size < 66000 ? file_size : 66000;
    unsigned char *buf = (unsigned char *)malloc((size_t)span);
    if (!buf) return -1;
    if (fseek(f, file_size - span, SEEK_SET) != 0) { free(buf); return -1; }
    if (fread(buf, 1, (size_t)span, f) != (size_t)span) { free(buf); return -1; }
    for (long i = span - 22; i >= 0; i--) {
        if (rd32(buf + i) == 0x06054b50) {
            *eocd_off = file_size - span + i;
            free(buf);
            return 0;
        }
    }
    free(buf);
    return -1;
}

wc_zip *wc_zip_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long eocd_off = 0;
    if (size < 22 || find_eocd(f, &eocd_off, size) != 0) { fclose(f); return NULL; }

    unsigned char eocd[22];
    fseek(f, eocd_off, SEEK_SET);
    if (fread(eocd, 1, 22, f) != 22) { fclose(f); return NULL; }
    int      count  = rd16(eocd + 10);
    uint32_t cd_size = rd32(eocd + 12);
    uint32_t cd_off  = rd32(eocd + 16);
    if (count <= 0) { fclose(f); return NULL; }

    unsigned char *cd = (unsigned char *)malloc(cd_size);
    if (!cd) { fclose(f); return NULL; }
    fseek(f, (long)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return NULL; }

    wc_zip *z = (wc_zip *)calloc(1, sizeof(*z));
    z->f = f;
    z->entries = (zentry *)calloc((size_t)count, sizeof(zentry));
    z->count = 0;

    uint32_t p = 0;
    for (int i = 0; i < count && p + 46 <= cd_size; i++) {
        if (rd32(cd + p) != 0x02014b50) break;
        uint16_t method  = rd16(cd + p + 10);
        uint32_t csize   = rd32(cd + p + 20);
        uint32_t usize   = rd32(cd + p + 24);
        uint16_t namelen = rd16(cd + p + 28);
        uint16_t extra   = rd16(cd + p + 30);
        uint16_t cmt     = rd16(cd + p + 32);
        uint32_t loc     = rd32(cd + p + 42);
        if (p + 46 + namelen > cd_size) break;
        zentry *e = &z->entries[z->count];
        e->name = (char *)malloc((size_t)namelen + 1);
        memcpy(e->name, cd + p + 46, namelen);
        e->name[namelen] = 0;
        e->method = method;
        e->comp_size = csize;
        e->uncomp_size = usize;
        e->local_off = loc;
        z->count++;
        p += 46u + namelen + extra + cmt;
    }
    free(cd);

    /* asset prefix from manifest.json — the engine asks for "main.lua",
     * the zip stores "assets/main.lua". Default matches the packer. */
    z->prefix = strdup("assets/");
    z->prefix_len = 7;
    char mbuf[2048];
    int n = wc_zip_read_entry(z, "manifest.json", mbuf, sizeof(mbuf) - 1);
    if (n > 0) {
        mbuf[n] = 0;
        const char *a = strstr(mbuf, "\"assets\"");
        if (a) {
            const char *q = strchr(a + 8, '"');
            if (q) {
                const char *q2 = strchr(q + 1, '"');
                if (q2 && q2 > q + 1) {
                    size_t len = (size_t)(q2 - q - 1);
                    free(z->prefix);
                    z->prefix = (char *)malloc(len + 2);
                    memcpy(z->prefix, q + 1, len);
                    z->prefix[len] = 0;
                    /* the packer writes "app/" or "assets/" with the slash;
                     * tolerate a manifest that omits it */
                    if (len && z->prefix[len - 1] != '/') { z->prefix[len] = '/'; z->prefix[len + 1] = 0; }
                    z->prefix_len = (unsigned)strlen(z->prefix);
                }
            }
        }
    }
    return z;
}

void wc_zip_close(wc_zip *z) {
    if (!z) return;
    for (int i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
    free(z->prefix);
    if (z->f) fclose(z->f);
    free(z);
}

static zentry *find_entry(wc_zip *z, const char *name) {
    for (int i = 0; i < z->count; i++)
        if (strcmp(z->entries[i].name, name) == 0) return &z->entries[i];
    return NULL;
}

/* Resolve an engine-facing asset name to a zip entry: try the prefix first
 * (the normal case), then the bare name so a flat cart still works. */
static zentry *resolve(wc_zip *z, const char *name, unsigned int name_len) {
    char full[1024];
    if (z->prefix_len + name_len + 1 > sizeof(full)) return NULL;
    memcpy(full, z->prefix, z->prefix_len);
    memcpy(full + z->prefix_len, name, name_len);
    full[z->prefix_len + name_len] = 0;
    zentry *e = find_entry(z, full);
    if (e) return e;
    if (name_len + 1 > sizeof(full)) return NULL;
    memcpy(full, name, name_len);
    full[name_len] = 0;
    return find_entry(z, full);
}

/* Read one entry's bytes. The local header repeats name/extra lengths and
 * they can differ from the central directory's — always re-read them. */
static int read_entry(wc_zip *z, zentry *e, void *buf, unsigned int cap) {
    if (!e) return -1;
    if (e->uncomp_size > cap) return -1;
    unsigned char lh[30];
    if (fseek(z->f, (long)e->local_off, SEEK_SET) != 0) return -1;
    if (fread(lh, 1, 30, z->f) != 30) return -1;
    if (rd32(lh) != 0x04034b50) return -1;
    long data_off = (long)e->local_off + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (fseek(z->f, data_off, SEEK_SET) != 0) return -1;

    if (e->method == 0) {                       /* stored */
        if (fread(buf, 1, e->uncomp_size, z->f) != e->uncomp_size) return -1;
        return (int)e->uncomp_size;
    }
    if (e->method != 8) return -1;              /* deflate only */

    unsigned char *cbuf = (unsigned char *)malloc(e->comp_size ? e->comp_size : 1);
    if (!cbuf) return -1;
    if (fread(cbuf, 1, e->comp_size, z->f) != e->comp_size) { free(cbuf); return -1; }

    z_stream s;
    memset(&s, 0, sizeof(s));
    s.next_in = cbuf;
    s.avail_in = e->comp_size;
    s.next_out = (unsigned char *)buf;
    s.avail_out = e->uncomp_size;
    /* -MAX_WBITS: raw deflate, no zlib wrapper (that is what zip stores) */
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) { free(cbuf); return -1; }
    int rc = inflate(&s, Z_FINISH);
    unsigned produced = (unsigned)s.total_out;
    inflateEnd(&s);
    free(cbuf);
    if (rc != Z_STREAM_END || produced != e->uncomp_size) return -1;
    return (int)produced;
}

int wc_zip_read_entry(wc_zip *z, const char *entry, void *buf, unsigned int cap) {
    return read_entry(z, find_entry(z, entry), buf, cap);
}

int wc_zip_asset_size(void *ctx, const char *name, unsigned int name_len) {
    wc_zip *z = (wc_zip *)ctx;
    zentry *e = resolve(z, name, name_len);
    return e ? (int)e->uncomp_size : -1;
}

int wc_zip_asset_read(void *ctx, const char *name, unsigned int name_len,
                      void *buf, unsigned int buf_len) {
    wc_zip *z = (wc_zip *)ctx;
    return read_entry(z, resolve(z, name, name_len), buf, buf_len);
}
