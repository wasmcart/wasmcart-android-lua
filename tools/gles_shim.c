/* gles_shim.c — macOS-harness-only GLSL translation.
 *
 * The engine's shaders are "#version 300 es" (GLES3), which is exactly right
 * on Android and rejected by macOS's desktop GL. Rather than fork the engine's
 * shader sources, intercept glShaderSource here and rewrite the version line
 * to "#version 330 core" on the way through. Desktop GLSL 1.30+ tolerates the
 * precision qualifiers that follow, so nothing else needs touching.
 *
 * THIS FILE IS NOT PART OF THE ANDROID BUILD. Android links real GLES3 where
 * the engine's shaders compile verbatim — which is the point of the port.
 */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef unsigned int GLuint;
typedef int GLsizei;
typedef int GLint;

/* The engine calls this; we translate and forward to the framework's. */
void glShaderSource(GLuint shader, GLsizei count,
                    const char *const *string, const GLint *length) {
    static void (*real)(GLuint, GLsizei, const char *const *, const GLint *);
    if (!real) real = dlsym(RTLD_NEXT, "glShaderSource");

    /* Concatenate the incoming sources so one rewrite covers a split shader
     * (the engine synthesizes a header + the cart's body for custom shaders). */
    size_t total = 0;
    for (int i = 0; i < count; i++)
        total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);

    char *buf = (char *)malloc(total + 64);
    size_t at = 0;
    for (int i = 0; i < count; i++) {
        size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
        memcpy(buf + at, string[i], n);
        at += n;
    }
    buf[at] = 0;

    const char *from = "#version 300 es";
    char *hit = strstr(buf, from);
    if (hit) {
        /* "#version 330 core" is one char longer than "#version 300 es" —
         * shift the tail rather than assuming in-place fits. */
        const char *to = "#version 330 core";
        size_t flen = strlen(from), tlen = strlen(to);
        memmove(hit + tlen, hit + flen, at - (size_t)(hit - buf) - flen + 1);
        memcpy(hit, to, tlen);
    }

    const char *one = buf;
    real(shader, 1, &one, NULL);
    free(buf);
}
