/* LD_PRELOAD shim for OpenGL-via-Zink + xfsr FSR.
 *
 * GL apps call glViewport(0,0, windowW, windowH) while caps-shrink made the
 * swapchain windowW/scale × windowH/scale. Remap full-window viewports/scissors.
 *
 * Wayland: Zink sizes the Vulkan swapchain from wl_egl_window, ignoring Vulkan
 * caps. Interpose wl_egl_window_create/resize to the render size so Zink asks
 * for a shrunk swapchain; the layer presents at the real window (DISPLAY hint).
 *
 * Must export interpose symbols (visibility default) — CMake builds with
 * -fvisibility=hidden. Also wrap glX/egl/glfw GetProcAddress so apps that
 * resolve GL via those (not the PLT) still get remapped viewports.
 *
 * Env (xfsr-run):
 *   XFSR_SCALE              — FSR scale (1.5 / 1.7 / 2.0)  [preferred]
 *   XFSR_RENDER_W/H         — optional absolute render size
 *   XFSR_DISPLAY_W/H        — optional absolute window size
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>

#define XFSR_EXPORT __attribute__((visibility("default")))

typedef int GLint;
typedef int GLsizei;

typedef void (*GlViewportFn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*GlScissorFn)(GLint, GLint, GLsizei, GLsizei);
typedef void (*GlViewportArrayvFn)(unsigned int, GLsizei, const float*);
typedef void (*GlScissorArrayvFn)(unsigned int, GLsizei, const GLint*);
typedef void (*GlViewportIndexedfFn)(unsigned int, float, float, float, float);
typedef void (*GlScissorIndexedFn)(unsigned int, GLint, GLint, GLsizei, GLsizei);
typedef void (*GlBlitFramebufferFn)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint,
                                    unsigned int, unsigned int);
typedef void (*GlGetIntegervFn)(unsigned int, GLint*);
typedef void* (*GetProcFn)(const char*);

static int g_dw, g_dh, g_rw, g_rh;
static float g_scale;
static int g_ready;
static int g_logged;
/* Last real glfw framebuffer size (window pixels), before we lie render size. */
static int g_fb_w;
static int g_fb_h;

static GlViewportFn real_glViewport;
static GlScissorFn real_glScissor;
static GlViewportArrayvFn real_glViewportArrayv;
static GlScissorArrayvFn real_glScissorArrayv;
static GlViewportIndexedfFn real_glViewportIndexedf;
static GlScissorIndexedFn real_glScissorIndexed;
static GlBlitFramebufferFn real_glBlitFramebuffer;
static GlGetIntegervFn real_glGetIntegerv;

/* Forward decls — wrap_proc / dlsym return these. */
XFSR_EXPORT void* glXGetProcAddress(const char* name);
XFSR_EXPORT void* glXGetProcAddressARB(const char* name);
XFSR_EXPORT void* eglGetProcAddress(const char* name);
XFSR_EXPORT void* glfwGetProcAddress(const char* name);
XFSR_EXPORT void* dlsym(void* handle, const char* symbol);
static void* resolve_next(const char* name, void* self);
static int query_egl_drawable(int* out_w, int* out_h);

static void load_extents(void) {
    /* Re-read every time — F1 preset changes layer scale; env may be updated by
     * a helper, and we must not latch launch ultra forever (postage-stamp after
     * recreate to a different render size). */
    const char* dw = getenv("XFSR_DISPLAY_W");
    const char* dh = getenv("XFSR_DISPLAY_H");
    const char* rw = getenv("XFSR_RENDER_W");
    const char* rh = getenv("XFSR_RENDER_H");
    const char* sc = getenv("XFSR_SCALE");
    g_dw = dw ? atoi(dw) : 0;
    g_dh = dh ? atoi(dh) : 0;
    g_rw = rw ? atoi(rw) : 0;
    g_rh = rh ? atoi(rh) : 0;
    g_scale = sc ? strtof(sc, NULL) : 0.f;
    if (g_scale < 1.01f) g_scale = 0.f;
    if (!g_ready && getenv("XFSR_VERBOSE")) {
        fprintf(stderr, "[xfsr-gl] shim scale=%.2f render=%dx%d display_hint=%dx%d\n",
                g_scale, g_rw, g_rh, g_dw, g_dh);
    }
    g_ready = 1;
}

/* Map a window/fb size to the Zink color-image (render) size.
 * Fullscreen matching XFSR_OUTPUT_* → absolute XFSR_RENDER_*.
 * Windowed (glmark2 800x600) → window/scale — never force 1080p's 768x432. */
static void target_render_size(int win_w, int win_h, int* out_rw, int* out_rh) {
    *out_rw = 0;
    *out_rh = 0;
    if (win_w < 64 || win_h < 64) return;
    const int near = 2;
    if (g_dw > 0 && g_dh > 0 && g_rw > 0 && g_rh > 0 && abs(win_w - g_dw) <= near &&
        abs(win_h - g_dh) <= near) {
        *out_rw = g_rw;
        *out_rh = g_rh;
        return;
    }
    if (g_scale >= 1.01f) {
        int rw = (int)(win_w / g_scale + 0.5f);
        int rh = (int)(win_h / g_scale + 0.5f);
        if (rw < 64) rw = 64;
        if (rh < 64) rh = 64;
        *out_rw = rw;
        *out_rh = rh;
    }
}

/* EGL drawable size = Zink color image. If it still equals the window, the layer
 * took the identity path (no caps-shrink) — remapping would postage-stamp.
 * Only remap when the drawable is already at render size. */
static int query_egl_drawable(int* out_w, int* out_h) {
    typedef void* (*EglGetCurrentDisplayFn)(void);
    typedef void* (*EglGetCurrentSurfaceFn)(unsigned int);
    typedef unsigned int (*EglQuerySurfaceFn)(void*, void*, int, int*);
    enum { EGL_DRAW = 0x3059, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056 };
    static EglGetCurrentDisplayFn get_dpy;
    static EglGetCurrentSurfaceFn get_surf;
    static EglQuerySurfaceFn query;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        get_dpy = (EglGetCurrentDisplayFn)resolve_next("eglGetCurrentDisplay", NULL);
        get_surf = (EglGetCurrentSurfaceFn)resolve_next("eglGetCurrentSurface", NULL);
        query = (EglQuerySurfaceFn)resolve_next("eglQuerySurface", NULL);
    }
    if (!get_dpy || !get_surf || !query) return 0;
    void* dpy = get_dpy();
    void* surf = get_surf(EGL_DRAW);
    if (!dpy || !surf) return 0;
    int w = 0, h = 0;
    if (!query(dpy, surf, EGL_WIDTH, &w) || !query(dpy, surf, EGL_HEIGHT, &h)) return 0;
    if (w < 64 || h < 64) return 0;
    *out_w = w;
    *out_h = h;
    return 1;
}

static void x11_note_remap_change(int lw, int lh);
static int x11_lie_wanted(void);

/* Layer sets XFSR_GL_REMAP=WxH when a proxy is live (same process). EGL on X11
 * Zink often still reports the window size while GetSwapchainImages are render-
 * sized — without this we skip remap and show a zoomed bottom-left crop. */
static int layer_remap_size(int* out_w, int* out_h) {
    const char* e = getenv("XFSR_GL_REMAP");
    if (!e || !*e) return 0;
    int rw = 0, rh = 0;
    if (sscanf(e, "%dx%d", &rw, &rh) != 2 && sscanf(e, "%d x %d", &rw, &rh) != 2) return 0;
    if (rw < 64 || rh < 64) return 0;
    *out_w = rw;
    *out_h = rh;
    return 1;
}

/* Map a coordinate from window pixels into a shrunk drawable / layer size. */
static int scale_axis(int v, int win, int dst) {
    if (win <= 0) return v;
    return (int)(((long)v * (long)dst + (win / 2)) / (long)win);
}

static void remap_rect(GLint* x, GLint* y, GLsizei* w, GLsizei* h) {
    load_extents();
    if (!w || !h) return;
    if (*w <= 0 || *h <= 0) return;
    const GLint ox = x ? *x : 0;
    const GLint oy = y ? *y : 0;
    const GLsizei ow = *w, oh = *h;

    /* Real window pixels — prefer last glfw/SDL window size before any lie. */
    const int near = 2;
    int win_w = (g_fb_w > 64) ? g_fb_w : 0;
    int win_h = (g_fb_h > 64) ? g_fb_h : 0;
    if (win_w < 64 && g_dw > 64) win_w = g_dw;
    if (win_h < 64 && g_dh > 64) win_h = g_dh;

    /* EGL drawable = Zink color image (tracks F1 recreate). Prefer it over
     * latched XFSR_SCALE — stale scale remaps to the wrong size and leaves a
     * postage-stamp in the corner of the new render image. */
    int dw = 0, dh = 0;
    const int have_egl = query_egl_drawable(&dw, &dh);
    int layer_w = 0, layer_h = 0;
    const int have_layer = layer_remap_size(&layer_w, &layer_h);
    if (have_layer) x11_note_remap_change(layer_w, layer_h);

    if ((have_egl || have_layer) && win_w < 64) {
        /* No window size yet — if this looks like a full-window rect, use it. */
        if (ox <= near && oy <= near && (int)*w >= 200 && (int)*h >= 200) {
            win_w = (int)*w;
            win_h = (int)*h;
        }
    }

    /* Prefer layer proxy size when EGL still looks window-sized (X11 Zink). */
    if (have_layer && win_w >= 64 && win_h >= 64 &&
        (layer_w + near < win_w || layer_h + near < win_h)) {
        const int egl_is_window =
            have_egl && abs(dw - win_w) <= near && abs(dh - win_h) <= near;
        const int egl_is_render =
            have_egl && abs(dw - layer_w) <= near && abs(dh - layer_h) <= near;
        if (!have_egl || egl_is_window || egl_is_render) {
            const int covers_win =
                (ox <= near && oy <= near && abs((int)*w - win_w) <= near &&
                 abs((int)*h - win_h) <= near);
            const int covers_display =
                (ox <= near && oy <= near && g_dw > 0 && g_dh > 0 &&
                 abs((int)*w - g_dw) <= near && abs((int)*h - g_dh) <= near);
            /* Already in render space (full drawable). */
            const int already_render =
                (ox <= near && oy <= near && abs((int)*w - layer_w) <= near &&
                 abs((int)*h - layer_h) <= near);
            if (already_render) return;
            if (covers_win || covers_display) {
                if (x) *x = 0;
                if (y) *y = 0;
                *w = layer_w;
                *h = layer_h;
                if (!g_logged && (*w != ow || *h != oh) && getenv("XFSR_VERBOSE")) {
                    g_logged = 1;
                    fprintf(stderr,
                            "[xfsr-gl] remap %dx%d → layer %dx%d (egl=%dx%d win=%dx%d)\n",
                            (int)ow, (int)oh, layer_w, layer_h, dw, dh, win_w, win_h);
                }
                return;
            }
            /* Godot/SDL letterbox & partial viewports: origin may be non-zero.
             * Large window-space content (integer scale / stretch blit) must fill
             * the render image — proportional scale would postage-stamp. */
            const int in_window =
                ox >= -near && oy >= -near && (int)ow >= 32 && (int)oh >= 32 &&
                ox + (int)ow <= win_w + near && oy + (int)oh <= win_h + near &&
                ((int)ow + near > layer_w || (int)oh + near > layer_h || ox > near ||
                 oy > near);
            if (in_window) {
                /* Window-space letterbox (both axes > render) or a fixed design
                 * viewport (BunnyForest 512² — may be taller than layer_h after
                 * caps-shrink): fill the render image so present is not a stamp. */
                const int letterbox_gt_layer =
                    (int)ow + near > layer_w && (int)oh + near > layer_h;
                /* ≤¾ of window on both axes (512² in 1280×720: 2× height fails). */
                const int small_vs_win =
                    (int)ow * 4 <= win_w * 3 && (int)oh * 4 <= win_h * 3;
                /* Origin or Godot top-left (GL y = win_h - height). */
                const int at_origin = ox <= near && oy <= near;
                const int at_top_left =
                    ox <= near && oh >= 32 && abs(oy + (int)oh - win_h) <= near + 1;
                /* After glfw/egl size lies, win≈layer so small_vs_win fails and a
                 * 512² viewport is left unscaled → bottom-left postage stamp. */
                const int squareish = abs((int)ow - (int)oh) <= near + 8;
                const int stamp_in_layer =
                    squareish && at_origin &&
                    ((int)ow + near < layer_w || (int)oh + near < layer_h);
                const int design_viewport =
                    ((small_vs_win && (at_origin || at_top_left)) || stamp_in_layer);
                if (letterbox_gt_layer || design_viewport) {
                    if (x) *x = 0;
                    if (y) *y = 0;
                    *w = layer_w;
                    *h = layer_h;
                    if (getenv("XFSR_VERBOSE") && (!g_logged || design_viewport)) {
                        g_logged = 1;
                        fprintf(stderr,
                                "[xfsr-gl] remap fill %d,%d %dx%d → 0,0 %dx%d "
                                "(win %dx%d%s)\n",
                                (int)ox, (int)oy, (int)ow, (int)oh, layer_w, layer_h, win_w,
                                win_h, design_viewport ? " design" : "");
                    }
                    return;
                }
                const GLint nx = (GLint)scale_axis(ox, win_w, layer_w);
                const GLint ny = (GLint)scale_axis(oy, win_h, layer_h);
                GLsizei nw = (GLsizei)scale_axis((int)ow, win_w, layer_w);
                GLsizei nh = (GLsizei)scale_axis((int)oh, win_h, layer_h);
                if (nw < 1) nw = 1;
                if (nh < 1) nh = 1;
                if (x) *x = nx;
                if (y) *y = ny;
                *w = nw;
                *h = nh;
                if (!g_logged && (nx != ox || ny != oy || nw != ow || nh != oh) &&
                    getenv("XFSR_VERBOSE")) {
                    g_logged = 1;
                    fprintf(stderr,
                            "[xfsr-gl] remap letterbox %d,%d %dx%d → %d,%d %dx%d "
                            "(layer %dx%d win %dx%d)\n",
                            (int)ox, (int)oy, (int)ow, (int)oh, (int)nx, (int)ny, (int)nw,
                            (int)nh, layer_w, layer_h, win_w, win_h);
                }
                return;
            }
        }
    }

    if (have_egl && win_w >= 64 && win_h >= 64) {
        /* Identity: drawable still window-sized and no live proxy → do not shrink. */
        if (abs(dw - win_w) <= near && abs(dh - win_h) <= near) return;

        /* Caps-shrunk drawable: scale window-space rects into the EGL image. */
        if (dw + near < win_w || dh + near < win_h) {
            const int covers_win =
                (ox <= near && oy <= near && abs((int)*w - win_w) <= near &&
                 abs((int)*h - win_h) <= near);
            const int covers_display =
                (ox <= near && oy <= near && g_dw > 0 && g_dh > 0 &&
                 abs((int)*w - g_dw) <= near && abs((int)*h - g_dh) <= near);
            const int covers_drawable =
                (ox <= near && oy <= near && (int)*w + near >= dw && (int)*h + near >= dh);
            if (covers_win || covers_display || covers_drawable) {
                if (x) *x = 0;
                if (y) *y = 0;
                *w = dw;
                *h = dh;
                if (!g_logged && (*w != ow || *h != oh) && getenv("XFSR_VERBOSE")) {
                    g_logged = 1;
                    fprintf(stderr,
                            "[xfsr-gl] remap %dx%d → egl %dx%d (glfw_fb=%dx%d win=%dx%d)\n",
                            (int)ow, (int)oh, dw, dh, g_fb_w, g_fb_h, win_w, win_h);
                }
                return;
            }
            const int in_window =
                ox >= -near && oy >= -near && (int)ow >= 32 && (int)oh >= 32 &&
                ox + (int)ow <= win_w + near && oy + (int)oh <= win_h + near &&
                ((int)ow + near > dw || (int)oh + near > dh || ox > near || oy > near);
            if (in_window) {
                if (x) *x = (GLint)scale_axis(ox, win_w, dw);
                if (y) *y = (GLint)scale_axis(oy, win_h, dh);
                *w = (GLsizei)scale_axis((int)ow, win_w, dw);
                *h = (GLsizei)scale_axis((int)oh, win_h, dh);
                if (*w < 1) *w = 1;
                if (*h < 1) *h = 1;
                return;
            }
        }
    }
}

typedef void* (*DlsymFn)(void*, const char*);
typedef void* (*DlvsymFn)(void*, const char*, const char*);
static DlsymFn real_dlsym;
static void ensure_dlsym(void);

/* libc/next dlsym — never our interposed symbol (same-DSO bind would recurse). */
static void* libc_dlsym(void* handle, const char* name) {
    ensure_dlsym();
    if (!real_dlsym) return NULL;
    return real_dlsym(handle, name);
}

static void* resolve_next(const char* name, void* self) {
    /* Prefer the next LD_PRELOAD (MangoHud_shim) so SwapBuffers / GetProc stay chained. */
    void* p = libc_dlsym(RTLD_NEXT, name);
    if (p && p != self) return p;
    void* gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!gl) gl = dlopen("libGL.so.1", RTLD_LAZY);
    if (gl) {
        p = libc_dlsym(gl, name);
        if (p && p != self) return p;
    }
    void* glx = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (!glx) glx = dlopen("libGLX.so.0", RTLD_LAZY);
    if (glx) {
        p = libc_dlsym(glx, name);
        if (p && p != self) return p;
    }
    void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!egl) egl = dlopen("libEGL.so.1", RTLD_LAZY);
    if (egl) {
        p = libc_dlsym(egl, name);
        if (p && p != self) return p;
    }
    return NULL;
}

static void stash_if_real(void** slot, void* candidate, void* wrapper) {
    if (!candidate || candidate == wrapper) return;
    if (!*slot) *slot = candidate;
}

/* Offscreen FBOs keep their design size (BunnyForest 512²). Only the default
 * framebuffer (Zink swapchain image) is remapped into the caps-shrunk layer. */
static int draw_fbo_is_default(void) {
    if (!real_glGetIntegerv)
        real_glGetIntegerv = (GlGetIntegervFn)resolve_next("glGetIntegerv", NULL);
    if (!real_glGetIntegerv) return 1;
    GLint drawFbo = 0;
    real_glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &drawFbo);
    return drawFbo == 0;
}

XFSR_EXPORT void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!real_glViewport)
        real_glViewport = (GlViewportFn)resolve_next("glViewport", (void*)glViewport);
    if (!real_glViewport) return;
    if (draw_fbo_is_default()) remap_rect(&x, &y, &width, &height);
    real_glViewport(x, y, width, height);
}

XFSR_EXPORT void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!real_glScissor)
        real_glScissor = (GlScissorFn)resolve_next("glScissor", (void*)glScissor);
    if (!real_glScissor) return;
    if (draw_fbo_is_default()) remap_rect(&x, &y, &width, &height);
    real_glScissor(x, y, width, height);
}

XFSR_EXPORT void glViewportIndexedf(unsigned int index, float x, float y, float w, float h) {
    if (!real_glViewportIndexedf)
        real_glViewportIndexedf =
            (GlViewportIndexedfFn)resolve_next("glViewportIndexedf", (void*)glViewportIndexedf);
    if (!real_glViewportIndexedf) return;
    if (index == 0 && draw_fbo_is_default()) {
        GLint ix = (GLint)x, iy = (GLint)y;
        GLsizei iw = (GLsizei)(w + 0.5f), ih = (GLsizei)(h + 0.5f);
        remap_rect(&ix, &iy, &iw, &ih);
        real_glViewportIndexedf(index, (float)ix, (float)iy, (float)iw, (float)ih);
        return;
    }
    real_glViewportIndexedf(index, x, y, w, h);
}

XFSR_EXPORT void glScissorIndexed(unsigned int index, GLint x, GLint y, GLsizei w, GLsizei h) {
    if (!real_glScissorIndexed)
        real_glScissorIndexed =
            (GlScissorIndexedFn)resolve_next("glScissorIndexed", (void*)glScissorIndexed);
    if (!real_glScissorIndexed) return;
    if (index == 0 && draw_fbo_is_default()) remap_rect(&x, &y, &w, &h);
    real_glScissorIndexed(index, x, y, w, h);
}

XFSR_EXPORT void glViewportArrayv(unsigned int first, GLsizei count, const float* v) {
    if (!real_glViewportArrayv)
        real_glViewportArrayv =
            (GlViewportArrayvFn)resolve_next("glViewportArrayv", (void*)glViewportArrayv);
    if (!real_glViewportArrayv || !v || count <= 0) {
        if (real_glViewportArrayv) real_glViewportArrayv(first, count, v);
        return;
    }
    if (first == 0 && draw_fbo_is_default()) {
        float tmp[4];
        memcpy(tmp, v, sizeof(tmp));
        GLint x = (GLint)tmp[0], y = (GLint)tmp[1];
        GLsizei w = (GLsizei)(tmp[2] + 0.5f), h = (GLsizei)(tmp[3] + 0.5f);
        remap_rect(&x, &y, &w, &h);
        tmp[0] = (float)x;
        tmp[1] = (float)y;
        tmp[2] = (float)w;
        tmp[3] = (float)h;
        if (count == 1) {
            real_glViewportArrayv(first, 1, tmp);
            return;
        }
        float* buf = (float*)malloc(sizeof(float) * 4 * (size_t)count);
        if (!buf) {
            real_glViewportArrayv(first, count, v);
            return;
        }
        memcpy(buf, v, sizeof(float) * 4 * (size_t)count);
        memcpy(buf, tmp, sizeof(tmp));
        real_glViewportArrayv(first, count, buf);
        free(buf);
        return;
    }
    real_glViewportArrayv(first, count, v);
}

XFSR_EXPORT void glScissorArrayv(unsigned int first, GLsizei count, const GLint* v) {
    if (!real_glScissorArrayv)
        real_glScissorArrayv =
            (GlScissorArrayvFn)resolve_next("glScissorArrayv", (void*)glScissorArrayv);
    if (!real_glScissorArrayv || !v || count <= 0) {
        if (real_glScissorArrayv) real_glScissorArrayv(first, count, v);
        return;
    }
    if (first == 0) {
        GLint tmp[4];
        memcpy(tmp, v, sizeof(tmp));
        remap_rect(&tmp[0], &tmp[1], &tmp[2], &tmp[3]);
        if (count == 1) {
            real_glScissorArrayv(first, 1, tmp);
            return;
        }
        GLint* buf = (GLint*)malloc(sizeof(GLint) * 4 * (size_t)count);
        if (!buf) {
            real_glScissorArrayv(first, count, v);
            return;
        }
        memcpy(buf, v, sizeof(GLint) * 4 * (size_t)count);
        memcpy(buf, tmp, sizeof(tmp));
        real_glScissorArrayv(first, count, buf);
        free(buf);
        return;
    }
    real_glScissorArrayv(first, count, v);
}

XFSR_EXPORT void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                   GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                   unsigned int mask, unsigned int filter) {
    if (!real_glBlitFramebuffer)
        real_glBlitFramebuffer =
            (GlBlitFramebufferFn)resolve_next("glBlitFramebuffer", (void*)glBlitFramebuffer);
    if (!real_glBlitFramebuffer) return;
    if (!real_glGetIntegerv)
        real_glGetIntegerv = (GlGetIntegervFn)resolve_next("glGetIntegerv", NULL);
    GLint drawFbo = 0;
    if (real_glGetIntegerv) real_glGetIntegerv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &drawFbo);
    /* Default FB = Zink swapchain image (render-sized under caps-shrink). Godot
     * stretch/letterbox blits often use non-zero origins at window coords — scale
     * the whole dest rect into drawable space (not only 0,0→W,H). */
    if (drawFbo == 0) {
        load_extents();
        int win_w = (g_fb_w > 64) ? g_fb_w : g_dw;
        int win_h = (g_fb_h > 64) ? g_fb_h : g_dh;
        int layer_w = 0, layer_h = 0;
        const int have_layer = layer_remap_size(&layer_w, &layer_h);
        int dw = 0, dh = 0;
        const int have_egl = query_egl_drawable(&dw, &dh);
        int dst_w = layer_w;
        int dst_h = layer_h;
        if (!have_layer || dst_w < 64 || dst_h < 64) {
            if (have_egl && dw >= 64 && dh >= 64) {
                dst_w = dw;
                dst_h = dh;
            } else {
                dst_w = 0;
                dst_h = 0;
            }
        }
        const int dx0 = dstX0 < dstX1 ? dstX0 : dstX1;
        const int dy0 = dstY0 < dstY1 ? dstY0 : dstY1;
        const int dx1 = dstX0 < dstX1 ? dstX1 : dstX0;
        const int dy1 = dstY0 < dstY1 ? dstY1 : dstY0;
        const int bw = dx1 - dx0;
        const int bh = dy1 - dy0;
        if (win_w >= 64 && win_h >= 64 && dst_w >= 64 && dst_h >= 64 &&
            (dst_w + 2 < win_w || dst_h + 2 < win_h) && bw > 32 && bh > 32 && dx0 >= -2 &&
            dy0 >= -2 && dx1 <= win_w + 2 && dy1 <= win_h + 2 &&
            (bw + 2 > dst_w || bh + 2 > dst_h || dx0 > 2 || dy0 > 2)) {
            /* Same rule as remap_rect: letterbox fill, design blit, or square
             * stamp left in a shrunk drawable after FB size lies. */
            const int letterbox_gt_dst = (bw + 2 > dst_w && bh + 2 > dst_h);
            const int design_blit = bw * 4 <= win_w * 3 && bh * 4 <= win_h * 3 && dx0 <= 2 &&
                                   (dy0 <= 2 || abs(dy0 + bh - win_h) <= 3);
            const int stamp_blit = abs(bw - bh) <= 10 && dx0 <= 2 && dy0 <= 2 &&
                                  (bw + 2 < dst_w || bh + 2 < dst_h);
            if (letterbox_gt_dst || design_blit || stamp_blit) {
                dstX0 = 0;
                dstY0 = 0;
                dstX1 = dst_w;
                dstY1 = dst_h;
            } else {
                dstX0 = scale_axis(dstX0, win_w, dst_w);
                dstY0 = scale_axis(dstY0, win_h, dst_h);
                dstX1 = scale_axis(dstX1, win_w, dst_w);
                dstY1 = scale_axis(dstY1, win_h, dst_h);
            }
            if (mask == 0x00004000u /* GL_COLOR_BUFFER_BIT */) {
                const char* ups = getenv("XFSR_UPSCALER");
                if (ups && (strcmp(ups, "nearest") == 0 || strcmp(ups, "nn") == 0 ||
                            strcmp(ups, "pixel") == 0))
                    filter = 0x2600u /* GL_NEAREST */;
                else
                    filter = 0x2601u /* GL_LINEAR */;
            }
        }
    }
    real_glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

static void remap_drawable_query(int* width, int* height) {
    if (!width || !height) return;
    /* Always latch the real window/drawable for remap math. */
    g_fb_w = *width;
    g_fb_h = *height;
    load_extents();
    /* Godot 3 (no X11 lie): keep reporting the real size so stretch/maximize
     * stay in sync with WSI. remap_rect maps window space → layer. */
    if (!x11_lie_wanted()) return;
    int lw = 0, lh = 0;
    if (layer_remap_size(&lw, &lh) && lw + 2 < g_fb_w && lh + 2 < g_fb_h) {
        *width = lw;
        *height = lh;
        return;
    }
    int dw = 0, dh = 0;
    if (!query_egl_drawable(&dw, &dh)) return;
    if (abs(dw - g_fb_w) <= 2 && abs(dh - g_fb_h) <= 2) return;
    if (dw + 2 < g_fb_w && dh + 2 < g_fb_h) {
        *width = dw;
        *height = dh;
    }
}

XFSR_EXPORT void glfwGetFramebufferSize(void* window, int* width, int* height) {
    typedef void (*Fn)(void*, int*, int*);
    static Fn real = NULL;
    if (!real) real = (Fn)resolve_next("glfwGetFramebufferSize", (void*)glfwGetFramebufferSize);
    if (!real) return;
    real(window, width, height);
    remap_drawable_query(width, height);
}

XFSR_EXPORT void SDL_GL_GetDrawableSize(void* window, int* width, int* height) {
    typedef void (*Fn)(void*, int*, int*);
    static Fn real = NULL;
    if (!real) real = (Fn)resolve_next("SDL_GL_GetDrawableSize", (void*)SDL_GL_GetDrawableSize);
    if (!real) return;
    real(window, width, height);
    remap_drawable_query(width, height);
}

XFSR_EXPORT int SDL_GetWindowSizeInPixels(void* window, int* width, int* height) {
    typedef int (*Fn)(void*, int*, int*);
    static Fn real = NULL;
    if (!real)
        real = (Fn)resolve_next("SDL_GetWindowSizeInPixels", (void*)SDL_GetWindowSizeInPixels);
    if (!real) return -1;
    /* Godot/SDL compare window vs drawable for stretch/letterbox. If window stays
     * at the real 1920×1080 while drawable is remapped to render size, content is
     * laid out for the large window and only fills a fraction of the swapchain.
     * Keep both lies in sync (real size cached in g_fb_* for proportional remap). */
    const int r = real(window, width, height);
    remap_drawable_query(width, height);
    return r;
}

XFSR_EXPORT void glfwGetWindowSize(void* window, int* width, int* height) {
    typedef void (*Fn)(void*, int*, int*);
    static Fn real = NULL;
    if (!real) real = (Fn)resolve_next("glfwGetWindowSize", (void*)glfwGetWindowSize);
    if (!real) return;
    real(window, width, height);
    /* Same as SDL: cache real size then report render size so window==drawable. */
    remap_drawable_query(width, height);
}

/* Publish the real toolkit window for the Vulkan layer present extent.
 * Overwrite edgerun's monitor-sized seed when it disagrees — otherwise windowed
 * / multi-monitor Zink presents at the wrong size (image does not fill). */
static void publish_window_hint(int win_w, int win_h, int rw, int rh) {
    char buf[32];
    if (win_w >= 64 && win_h >= 64) {
        g_fb_w = win_w;
        g_fb_h = win_h;
        int overwrite = 1;
        const char* edw = getenv("XFSR_DISPLAY_W");
        const char* edh = getenv("XFSR_DISPLAY_H");
        if (edw && edh && *edw && *edh) {
            const int dw = atoi(edw), dh = atoi(edh);
            if (abs(dw - win_w) <= 2 && abs(dh - win_h) <= 2) overwrite = 0;
        }
        if (overwrite) {
            snprintf(buf, sizeof(buf), "%d", win_w);
            setenv("XFSR_DISPLAY_W", buf, 1);
            snprintf(buf, sizeof(buf), "%d", win_h);
            setenv("XFSR_DISPLAY_H", buf, 1);
            snprintf(buf, sizeof(buf), "%dx%d", win_w, win_h);
            setenv("XFSR_DISPLAY_EXTENT", buf, 1);
        }
    }
    if (rw >= 64 && rh >= 64) {
        int overwrite = 1;
        const char* erw = getenv("XFSR_RENDER_W");
        const char* erh = getenv("XFSR_RENDER_H");
        if (erw && erh && *erw && *erh) {
            const int crw = atoi(erw), crh = atoi(erh);
            if (abs(crw - rw) <= 2 && abs(crh - rh) <= 2) overwrite = 0;
        }
        if (overwrite) {
            snprintf(buf, sizeof(buf), "%d", rw);
            setenv("XFSR_RENDER_W", buf, 1);
            snprintf(buf, sizeof(buf), "%d", rh);
            setenv("XFSR_RENDER_H", buf, 1);
        }
    }
}

static int shrink_wl_size(int* width, int* height) {
    if (!width || !height) return 0;
    const int win_w = *width, win_h = *height;
    if (win_w < 64 || win_h < 64) return 0;
    load_extents();
    int rw = 0, rh = 0;
    target_render_size(win_w, win_h, &rw, &rh);
    if (rw <= 0 || rh <= 0) return 0;
    if (rw >= win_w && rh >= win_h) return 0;
    publish_window_hint(win_w, win_h, rw, rh);
    *width = rw;
    *height = rh;
    if (getenv("XFSR_VERBOSE")) {
        fprintf(stderr, "[xfsr-gl] wl_egl_window %dx%d → render %dx%d (Wayland Zink shrink)\n",
                win_w, win_h, rw, rh);
    }
    return 1;
}

typedef struct wl_egl_window WlEglWindow;
typedef struct wl_surface WlSurface;
typedef WlEglWindow* (*WlEglCreateFn)(WlSurface*, int, int);
typedef void (*WlEglResizeFn)(WlEglWindow*, int, int, int, int);

static void* wayland_egl_sym(const char* name, void* self) {
    void* p = resolve_next(name, self);
    if (p && p != self) return p;
    void* so = dlopen("libwayland-egl.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (!so) so = dlopen("libwayland-egl.so.1", RTLD_LAZY);
    if (!so) return NULL;
    p = libc_dlsym(so, name);
    return (p && p != self) ? p : NULL;
}

XFSR_EXPORT WlEglWindow* wl_egl_window_create(WlSurface* surface, int width, int height) {
    static WlEglCreateFn real = NULL;
    if (!real)
        real = (WlEglCreateFn)wayland_egl_sym("wl_egl_window_create", (void*)wl_egl_window_create);
    if (!real) return NULL;
    shrink_wl_size(&width, &height);
    return real(surface, width, height);
}

XFSR_EXPORT void wl_egl_window_resize(WlEglWindow* egl_window, int width, int height, int dx,
                                      int dy) {
    static WlEglResizeFn real = NULL;
    if (!real)
        real = (WlEglResizeFn)wayland_egl_sym("wl_egl_window_resize", (void*)wl_egl_window_resize);
    if (!real) return;
    shrink_wl_size(&width, &height);
    real(egl_window, width, height, dx, dy);
}

/* ---- X11 size lies (Godot DisplayServerX11 via sowrap dlsym) ----
 * Report render size so stretch-mode Godot fills the caps-shrunk Zink image.
 * XFSR_X11_LIE=0|1 overrides. Auto: ON when the main EXE does not DT_NEEDED
 * libX11 (Godot 4 sowrap / MMX); OFF when it links libX11 (Godot 3 / BunnyForest
 * fixed viewports — lying postage-stamps on resize). */

static int exe_dt_needed_libx11(void) {
    const int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return 0;
    Elf64_Ehdr eh;
    int linked = 0;
    if (read(fd, &eh, sizeof(eh)) != (ssize_t)sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64) {
        close(fd);
        return 0;
    }
    if (lseek(fd, (off_t)eh.e_phoff, SEEK_SET) < 0) {
        close(fd);
        return 0;
    }
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (read(fd, &ph, sizeof(ph)) != (ssize_t)sizeof(ph)) break;
        if (ph.p_type != PT_DYNAMIC) continue;
        const size_t n = (size_t)ph.p_filesz / sizeof(Elf64_Dyn);
        Elf64_Dyn* dyn = (Elf64_Dyn*)malloc(ph.p_filesz);
        if (!dyn) break;
        if (lseek(fd, (off_t)ph.p_offset, SEEK_SET) < 0 ||
            read(fd, dyn, ph.p_filesz) != (ssize_t)ph.p_filesz) {
            free(dyn);
            break;
        }
        Elf64_Addr strtab = 0;
        for (size_t j = 0; j < n; ++j) {
            if (dyn[j].d_tag == DT_STRTAB) strtab = dyn[j].d_un.d_ptr;
        }
        /* STRTAB is a VA — on PIE use /proc/self/maps base; fall back to file off via DT_STRSZ walk. */
        char* strs = NULL;
        size_t strsz = 0;
        for (size_t j = 0; j < n; ++j) {
            if (dyn[j].d_tag == DT_STRSZ) strsz = (size_t)dyn[j].d_un.d_val;
        }
        /* Locate STRTAB file offset: prefer matching PT_LOAD containing strtab VA. */
        Elf64_Off stroff = 0;
        int found_str = 0;
        if (lseek(fd, (off_t)eh.e_phoff, SEEK_SET) >= 0) {
            for (uint16_t k = 0; k < eh.e_phnum; ++k) {
                Elf64_Phdr lp;
                if (read(fd, &lp, sizeof(lp)) != (ssize_t)sizeof(lp)) break;
                if (lp.p_type != PT_LOAD) continue;
                if (strtab >= lp.p_vaddr && strtab < lp.p_vaddr + lp.p_filesz) {
                    stroff = lp.p_offset + (strtab - lp.p_vaddr);
                    found_str = 1;
                    break;
                }
            }
        }
        if (found_str && strsz > 0 && strsz < 1 << 20) {
            strs = (char*)malloc(strsz);
            if (strs && lseek(fd, (off_t)stroff, SEEK_SET) >= 0 &&
                read(fd, strs, strsz) == (ssize_t)strsz) {
                for (size_t j = 0; j < n; ++j) {
                    if (dyn[j].d_tag != DT_NEEDED) continue;
                    const size_t off = (size_t)dyn[j].d_un.d_val;
                    if (off < strsz && strstr(strs + off, "libX11.so")) {
                        linked = 1;
                        break;
                    }
                }
            }
            free(strs);
        }
        free(dyn);
        break;
    }
    close(fd);
    return linked;
}

static int x11_lie_wanted(void) {
    const char* e = getenv("XFSR_X11_LIE");
    if (e && e[0] == '0') return 0;
    if (e && e[0] == '1') return 1;
    static int cached = -1;
    if (cached < 0) {
        /* libX11 linked (Godot 3 / BunnyForest): never lie — WSI tracks the real
         * window, so lying desyncs on maximize/fullscreen (stamp + overlay in
         * the old rect). No libX11 (Godot 4 sowrap / MMX): lie so stretch fills
         * the caps-shrunk image. */
        cached = exe_dt_needed_libx11() ? 0 : 1;
        if (getenv("XFSR_VERBOSE"))
            fprintf(stderr, "[xfsr-gl] X11 lie auto=%s (exe %s libX11)\n", cached ? "on" : "off",
                    cached ? "without" : "links");
    }
    return cached;
}

static int x11_size_is_game_window(int real_w, int real_h, int lw, int lh) {
    const int near = 8;
    load_extents();
    /* Match latched game fb / display hint — never the X root (monitor sum). */
    if (g_fb_w >= 64 && abs(real_w - g_fb_w) <= near && abs(real_h - g_fb_h) <= near)
        return 1;
    if (g_dw >= 64 && abs(real_w - g_dw) <= near && abs(real_h - g_dh) <= near) return 1;
    if (g_scale >= 1.01f) {
        const int expect_w = (int)((float)lw * g_scale + 0.5f);
        const int expect_h = (int)((float)lh * g_scale + 0.5f);
        if (abs(real_w - expect_w) <= near && abs(real_h - expect_h) <= near) return 1;
    }
    /* Fullscreen-ish window: same aspect as render image, not multi-monitor wide. */
    if (lw >= 64 && lh >= 64 && real_w > lw && real_h > lh) {
        const long aspect_r = (long)real_w * (long)lh;
        const long aspect_l = (long)lw * (long)real_h;
        const int aspect_ok = aspect_r > aspect_l - aspect_l / 20 &&
                              aspect_r < aspect_l + aspect_l / 20;
        const int not_desktop = real_w < lw * 3 && real_h < lh * 3;
        if (aspect_ok && not_desktop) return 1;
    }
    return 0;
}

static int x11_lie_size(int real_w, int real_h, int* out_w, int* out_h) {
    if (real_w < 64 || real_h < 64) return 0;
    /* Always latch the real window — needed for remap after resize/fullscreen. */
    g_fb_w = real_w;
    g_fb_h = real_h;
    if (!x11_lie_wanted()) {
        publish_window_hint(real_w, real_h, 0, 0);
        return 0;
    }
    load_extents();
    int lw = 0, lh = 0;
    /* Only while a proxy is live — never scale-lie early desktop/root queries. */
    if (!layer_remap_size(&lw, &lh) || lw + 2 >= real_w || lh + 2 >= real_h) return 0;
    if (!x11_size_is_game_window(real_w, real_h, lw, lh)) return 0;
    publish_window_hint(real_w, real_h, lw, lh);
    *out_w = lw;
    *out_h = lh;
    return 1;
}

static void x11_scale_xy(int* x, int* y) {
    if (!x11_lie_wanted()) return;
    if (!x || !y || g_fb_w < 64 || g_fb_h < 64) return;
    int lw = 0, lh = 0;
    if (!layer_remap_size(&lw, &lh) || lw + 2 >= g_fb_w || lh + 2 >= g_fb_h) {
        load_extents();
        target_render_size(g_fb_w, g_fb_h, &lw, &lh);
        if (lw < 64 || lh < 64 || lw + 2 >= g_fb_w) return;
    }
    *x = scale_axis(*x, g_fb_w, lw);
    *y = scale_axis(*y, g_fb_h, lh);
}

static void x11_patch_event(XEvent* ev) {
    if (!ev) return;
    switch (ev->type) {
    case ConfigureNotify: {
        /* Always track real size for GL remap; only rewrite event when lying. */
        int lw = 0, lh = 0;
        const int real_w = ev->xconfigure.width, real_h = ev->xconfigure.height;
        if (real_w >= 64 && real_h >= 64) {
            if (abs(real_w - g_fb_w) > 8 || abs(real_h - g_fb_h) > 8)
                g_logged = 0; /* re-log remap after maximize/fullscreen */
            g_fb_w = real_w;
            g_fb_h = real_h;
        }
        if (x11_lie_size(real_w, real_h, &lw, &lh)) {
            ev->xconfigure.width = lw;
            ev->xconfigure.height = lh;
        }
        break;
    }
    case MotionNotify:
        x11_scale_xy(&ev->xmotion.x, &ev->xmotion.y);
        break;
    case ButtonPress:
    case ButtonRelease:
        x11_scale_xy(&ev->xbutton.x, &ev->xbutton.y);
        break;
    case EnterNotify:
    case LeaveNotify:
        x11_scale_xy(&ev->xcrossing.x, &ev->xcrossing.y);
        break;
    default:
        break;
    }
}

static void* x11_sym(const char* name, void* self) {
    void* p = resolve_next(name, self);
    if (p && p != self) return p;
    void* so = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
    if (!so) so = dlopen("libX11.so.6", RTLD_LAZY);
    if (!so) return NULL;
    p = libc_dlsym(so, name);
    return (p && p != self) ? p : NULL;
}

typedef Status (*XGetGeometryFn)(Display*, Drawable, Window*, int*, int*, unsigned int*,
                                 unsigned int*, unsigned int*, unsigned int*);
typedef Status (*XGetWindowAttributesFn)(Display*, Window, XWindowAttributes*);
typedef int (*XNextEventFn)(Display*, XEvent*);
typedef int (*XPeekEventFn)(Display*, XEvent*);
typedef int (*XWindowEventFn)(Display*, Window, long, XEvent*);
typedef int (*XMaskEventFn)(Display*, long, XEvent*);
typedef int (*XCheckMaskEventFn)(Display*, long, XEvent*);
typedef int (*XCheckWindowEventFn)(Display*, Window, long, XEvent*);
typedef int (*XCheckTypedEventFn)(Display*, int, XEvent*);
typedef int (*XCheckTypedWindowEventFn)(Display*, Window, int, XEvent*);
typedef int (*XIfEventFn)(Display*, XEvent*, Bool (*)(Display*, XEvent*, XPointer), XPointer);
typedef int (*XCheckIfEventFn)(Display*, XEvent*, Bool (*)(Display*, XEvent*, XPointer), XPointer);
typedef Bool (*XQueryPointerFn)(Display*, Window, Window*, Window*, int*, int*, int*, int*,
                                unsigned int*);

typedef int (*XResizeWindowFn)(Display*, Window, unsigned int, unsigned int);
typedef int (*XFlushFn)(Display*);

static XGetGeometryFn real_XGetGeometry;
static XGetWindowAttributesFn real_XGetWindowAttributes;
static XNextEventFn real_XNextEvent;
static XPeekEventFn real_XPeekEvent;
static XWindowEventFn real_XWindowEvent;
static XMaskEventFn real_XMaskEvent;
static XCheckMaskEventFn real_XCheckMaskEvent;
static XCheckWindowEventFn real_XCheckWindowEvent;
static XCheckTypedEventFn real_XCheckTypedEvent;
static XCheckTypedWindowEventFn real_XCheckTypedWindowEvent;
static XIfEventFn real_XIfEvent;
static XCheckIfEventFn real_XCheckIfEvent;
static XQueryPointerFn real_XQueryPointer;
static XResizeWindowFn real_XResizeWindow;
static XFlushFn real_XFlush;
static int g_x11_lie_logged;
static int g_x11_configure_kicked;
static int g_x11_output_bumped;
static int g_kick_lw, g_kick_lh;
static int g_last_remap_w, g_last_remap_h;
static Display* g_x11_dpy;
static Window g_x11_game_win;

/* When edgerun --output is larger than the current window, grow the X11 window
 * once so Vulkan WSI can present at that extent (needed for 4K on a 1080p panel). */
static void x11_maybe_bump_output(Display* dpy, Window w, int real_w, int real_h) {
    if (g_x11_output_bumped || !dpy || !w || real_w < 64 || real_h < 64) return;
    load_extents();
    int out_w = g_dw, out_h = g_dh;
    if (out_w < 64 || out_h < 64) {
        const char* ew = getenv("XFSR_OUTPUT_W");
        const char* eh = getenv("XFSR_OUTPUT_H");
        if (ew && eh && *ew && *eh) {
            out_w = atoi(ew);
            out_h = atoi(eh);
        }
    }
    if (out_w < 64 || out_h < 64) return;
    /* Only grow — never shrink the user's window via --output. */
    if (out_w <= real_w + 8 && out_h <= real_h + 8) return;
    /* Skip root / multi-monitor desktop span (e.g. 3840×1200 across two panels)
     * both as the current window and as the --output / display hint target. */
    if (real_w > out_w + 8 || real_h * 2 < real_w || out_h * 2 < out_w) return;
    if (!real_XResizeWindow)
        real_XResizeWindow = (XResizeWindowFn)x11_sym("XResizeWindow", NULL);
    if (!real_XFlush) real_XFlush = (XFlushFn)x11_sym("XFlush", NULL);
    if (!real_XResizeWindow) return;
    g_x11_output_bumped = 1;
    real_XResizeWindow(dpy, w, (unsigned int)out_w, (unsigned int)out_h);
    if (real_XFlush) real_XFlush(dpy);
    if (getenv("XFSR_VERBOSE"))
        fprintf(stderr, "[xfsr-gl] X11 bump window %dx%d → output %dx%d\n", real_w, real_h, out_w,
                out_h);
}

/* Bounce 1px so Godot picks up the lied size via ConfigureNotify (needed once
 * when the proxy appears). Skip re-kick on remap changes — live F1 rescale is
 * unsupported on X11-lie titles. XFSR_X11_RESIZE_KICK=0 disables. */
static void x11_kick_configure(Display* dpy, Window w, const XWindowAttributes* attrs, int lw,
                               int lh) {
    if (!dpy || !w || !attrs || lw < 64 || lh < 64) return;
    if (!x11_lie_wanted()) return;
    const char* kick = getenv("XFSR_X11_RESIZE_KICK");
    if (kick && kick[0] == '0') return;
    /* One-shot only — do not re-kick when F1 changes remap (postage-stamps). */
    if (g_x11_configure_kicked) return;
    (void)lh;
    (void)g_kick_lw;
    (void)g_kick_lh;
    if (!real_XResizeWindow)
        real_XResizeWindow = (XResizeWindowFn)x11_sym("XResizeWindow", NULL);
    if (!real_XFlush) real_XFlush = (XFlushFn)x11_sym("XFlush", NULL);
    if (!real_XResizeWindow) return;
    const unsigned int rw = (unsigned int)attrs->width;
    const unsigned int rh = (unsigned int)attrs->height;
    if (rw < 65 || rh < 64) return;
    g_x11_dpy = dpy;
    g_x11_game_win = w;
    g_x11_configure_kicked = 1;
    g_kick_lw = lw;
    g_kick_lh = lh;
    real_XResizeWindow(dpy, w, rw - 1u, rh);
    real_XResizeWindow(dpy, w, rw, rh);
    if (real_XFlush) real_XFlush(dpy);
    if (getenv("XFSR_VERBOSE"))
        fprintf(stderr, "[xfsr-gl] X11 resize-kick for lie %dx%d (real %dx%d)\n", lw, lh, (int)rw,
                (int)rh);
}

/* Track remap size for logging; do not re-kick (live F1 rescale postage-stamps). */
static void x11_note_remap_change(int lw, int lh) {
    if (lw < 64 || lh < 64) return;
    if (lw == g_last_remap_w && lh == g_last_remap_h) return;
    g_last_remap_w = lw;
    g_last_remap_h = lh;
    g_logged = 0;
}

XFSR_EXPORT Status XGetGeometry(Display* dpy, Drawable d, Window* root, int* x, int* y,
                                unsigned int* width, unsigned int* height, unsigned int* border,
                                unsigned int* depth) {
    if (!real_XGetGeometry)
        real_XGetGeometry = (XGetGeometryFn)x11_sym("XGetGeometry", (void*)XGetGeometry);
    if (!real_XGetGeometry) return 0;
    Status st = real_XGetGeometry(dpy, d, root, x, y, width, height, border, depth);
    if (st && width && height) {
        int lw = 0, lh = 0;
        if (x11_lie_size((int)*width, (int)*height, &lw, &lh)) {
            *width = (unsigned int)lw;
            *height = (unsigned int)lh;
            if (!g_x11_lie_logged && getenv("XFSR_VERBOSE")) {
                g_x11_lie_logged = 1;
                fprintf(stderr, "[xfsr-gl] X11 size lie %dx%d → %dx%d\n", g_fb_w, g_fb_h, lw, lh);
            }
        }
    }
    return st;
}

XFSR_EXPORT Status XGetWindowAttributes(Display* dpy, Window w, XWindowAttributes* attrs) {
    if (!real_XGetWindowAttributes)
        real_XGetWindowAttributes =
            (XGetWindowAttributesFn)x11_sym("XGetWindowAttributes", (void*)XGetWindowAttributes);
    if (!real_XGetWindowAttributes) return 0;
    Status st = real_XGetWindowAttributes(dpy, w, attrs);
    if (st && attrs) {
        int lw = 0, lh = 0;
        const int real_w = attrs->width, real_h = attrs->height;
        x11_maybe_bump_output(dpy, w, real_w, real_h);
        if (x11_lie_size(real_w, real_h, &lw, &lh)) {
            g_x11_dpy = dpy;
            g_x11_game_win = w;
            x11_kick_configure(dpy, w, attrs, lw, lh);
            attrs->width = lw;
            attrs->height = lh;
            if (!g_x11_lie_logged && getenv("XFSR_VERBOSE")) {
                g_x11_lie_logged = 1;
                fprintf(stderr, "[xfsr-gl] X11 attrs lie → %dx%d (real %dx%d)\n", lw, lh, real_w,
                        real_h);
            }
        }
    }
    return st;
}

XFSR_EXPORT int XNextEvent(Display* dpy, XEvent* ev) {
    if (!real_XNextEvent) real_XNextEvent = (XNextEventFn)x11_sym("XNextEvent", (void*)XNextEvent);
    if (!real_XNextEvent) return 0;
    int r = real_XNextEvent(dpy, ev);
    x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XPeekEvent(Display* dpy, XEvent* ev) {
    if (!real_XPeekEvent) real_XPeekEvent = (XPeekEventFn)x11_sym("XPeekEvent", (void*)XPeekEvent);
    if (!real_XPeekEvent) return 0;
    int r = real_XPeekEvent(dpy, ev);
    x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XWindowEvent(Display* dpy, Window w, long mask, XEvent* ev) {
    if (!real_XWindowEvent)
        real_XWindowEvent = (XWindowEventFn)x11_sym("XWindowEvent", (void*)XWindowEvent);
    if (!real_XWindowEvent) return 0;
    int r = real_XWindowEvent(dpy, w, mask, ev);
    x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XMaskEvent(Display* dpy, long mask, XEvent* ev) {
    if (!real_XMaskEvent) real_XMaskEvent = (XMaskEventFn)x11_sym("XMaskEvent", (void*)XMaskEvent);
    if (!real_XMaskEvent) return 0;
    int r = real_XMaskEvent(dpy, mask, ev);
    x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XCheckMaskEvent(Display* dpy, long mask, XEvent* ev) {
    if (!real_XCheckMaskEvent)
        real_XCheckMaskEvent = (XCheckMaskEventFn)x11_sym("XCheckMaskEvent", (void*)XCheckMaskEvent);
    if (!real_XCheckMaskEvent) return 0;
    int r = real_XCheckMaskEvent(dpy, mask, ev);
    if (r) x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XCheckWindowEvent(Display* dpy, Window w, long mask, XEvent* ev) {
    if (!real_XCheckWindowEvent)
        real_XCheckWindowEvent =
            (XCheckWindowEventFn)x11_sym("XCheckWindowEvent", (void*)XCheckWindowEvent);
    if (!real_XCheckWindowEvent) return 0;
    int r = real_XCheckWindowEvent(dpy, w, mask, ev);
    if (r) x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XCheckTypedEvent(Display* dpy, int type, XEvent* ev) {
    if (!real_XCheckTypedEvent)
        real_XCheckTypedEvent =
            (XCheckTypedEventFn)x11_sym("XCheckTypedEvent", (void*)XCheckTypedEvent);
    if (!real_XCheckTypedEvent) return 0;
    int r = real_XCheckTypedEvent(dpy, type, ev);
    if (r) x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XCheckTypedWindowEvent(Display* dpy, Window w, int type, XEvent* ev) {
    if (!real_XCheckTypedWindowEvent)
        real_XCheckTypedWindowEvent = (XCheckTypedWindowEventFn)x11_sym(
            "XCheckTypedWindowEvent", (void*)XCheckTypedWindowEvent);
    if (!real_XCheckTypedWindowEvent) return 0;
    int r = real_XCheckTypedWindowEvent(dpy, w, type, ev);
    if (r) x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XIfEvent(Display* dpy, XEvent* ev, Bool (*pred)(Display*, XEvent*, XPointer),
                         XPointer arg) {
    if (!real_XIfEvent) real_XIfEvent = (XIfEventFn)x11_sym("XIfEvent", (void*)XIfEvent);
    if (!real_XIfEvent) return 0;
    int r = real_XIfEvent(dpy, ev, pred, arg);
    x11_patch_event(ev);
    return r;
}

XFSR_EXPORT int XCheckIfEvent(Display* dpy, XEvent* ev, Bool (*pred)(Display*, XEvent*, XPointer),
                              XPointer arg) {
    if (!real_XCheckIfEvent)
        real_XCheckIfEvent = (XCheckIfEventFn)x11_sym("XCheckIfEvent", (void*)XCheckIfEvent);
    if (!real_XCheckIfEvent) return 0;
    int r = real_XCheckIfEvent(dpy, ev, pred, arg);
    if (r) x11_patch_event(ev);
    return r;
}

XFSR_EXPORT Bool XQueryPointer(Display* dpy, Window w, Window* root, Window* child, int* root_x,
                               int* root_y, int* win_x, int* win_y, unsigned int* mask) {
    if (!real_XQueryPointer)
        real_XQueryPointer = (XQueryPointerFn)x11_sym("XQueryPointer", (void*)XQueryPointer);
    if (!real_XQueryPointer) return False;
    Bool r = real_XQueryPointer(dpy, w, root, child, root_x, root_y, win_x, win_y, mask);
    if (r) x11_scale_xy(win_x, win_y);
    return r;
}

static void* wrap_proc(const char* name, void* real) {
    if (!name) return real;
    /* glmark2 dlopens libGL and dlsyms these by name — LD_PRELOAD alone never sees them. */
    if (strcmp(name, "glXGetProcAddress") == 0) return (void*)glXGetProcAddress;
    if (strcmp(name, "glXGetProcAddressARB") == 0) return (void*)glXGetProcAddressARB;
    if (strcmp(name, "eglGetProcAddress") == 0) return (void*)eglGetProcAddress;
    if (strcmp(name, "glfwGetProcAddress") == 0) return (void*)glfwGetProcAddress;
    if (strcmp(name, "wl_egl_window_create") == 0) return (void*)wl_egl_window_create;
    if (strcmp(name, "wl_egl_window_resize") == 0) return (void*)wl_egl_window_resize;
    if (strcmp(name, "SDL_GL_GetDrawableSize") == 0) return (void*)SDL_GL_GetDrawableSize;
    if (strcmp(name, "SDL_GetWindowSizeInPixels") == 0) return (void*)SDL_GetWindowSizeInPixels;
    /* Godot DisplayServerX11 sowrap: dlsym these from libX11.so.6. */
    if (real && strcmp(name, "XGetGeometry") == 0) {
        stash_if_real((void**)&real_XGetGeometry, real, (void*)XGetGeometry);
        return (void*)XGetGeometry;
    }
    if (real && strcmp(name, "XGetWindowAttributes") == 0) {
        stash_if_real((void**)&real_XGetWindowAttributes, real, (void*)XGetWindowAttributes);
        return (void*)XGetWindowAttributes;
    }
    if (real && strcmp(name, "XNextEvent") == 0) {
        stash_if_real((void**)&real_XNextEvent, real, (void*)XNextEvent);
        return (void*)XNextEvent;
    }
    if (real && strcmp(name, "XPeekEvent") == 0) {
        stash_if_real((void**)&real_XPeekEvent, real, (void*)XPeekEvent);
        return (void*)XPeekEvent;
    }
    if (real && strcmp(name, "XWindowEvent") == 0) {
        stash_if_real((void**)&real_XWindowEvent, real, (void*)XWindowEvent);
        return (void*)XWindowEvent;
    }
    if (real && strcmp(name, "XMaskEvent") == 0) {
        stash_if_real((void**)&real_XMaskEvent, real, (void*)XMaskEvent);
        return (void*)XMaskEvent;
    }
    if (real && strcmp(name, "XCheckMaskEvent") == 0) {
        stash_if_real((void**)&real_XCheckMaskEvent, real, (void*)XCheckMaskEvent);
        return (void*)XCheckMaskEvent;
    }
    if (real && strcmp(name, "XCheckWindowEvent") == 0) {
        stash_if_real((void**)&real_XCheckWindowEvent, real, (void*)XCheckWindowEvent);
        return (void*)XCheckWindowEvent;
    }
    if (real && strcmp(name, "XCheckTypedEvent") == 0) {
        stash_if_real((void**)&real_XCheckTypedEvent, real, (void*)XCheckTypedEvent);
        return (void*)XCheckTypedEvent;
    }
    if (real && strcmp(name, "XCheckTypedWindowEvent") == 0) {
        stash_if_real((void**)&real_XCheckTypedWindowEvent, real, (void*)XCheckTypedWindowEvent);
        return (void*)XCheckTypedWindowEvent;
    }
    if (real && strcmp(name, "XIfEvent") == 0) {
        stash_if_real((void**)&real_XIfEvent, real, (void*)XIfEvent);
        return (void*)XIfEvent;
    }
    if (real && strcmp(name, "XCheckIfEvent") == 0) {
        stash_if_real((void**)&real_XCheckIfEvent, real, (void*)XCheckIfEvent);
        return (void*)XCheckIfEvent;
    }
    if (real && strcmp(name, "XQueryPointer") == 0) {
        stash_if_real((void**)&real_XQueryPointer, real, (void*)XQueryPointer);
        return (void*)XQueryPointer;
    }
    if (!real) return real;
    if (strcmp(name, "glBlitFramebuffer") == 0) {
        stash_if_real((void**)&real_glBlitFramebuffer, real, (void*)glBlitFramebuffer);
        return (void*)glBlitFramebuffer;
    }
    if (strcmp(name, "glViewport") == 0) {
        stash_if_real((void**)&real_glViewport, real, (void*)glViewport);
        return (void*)glViewport;
    }
    if (strcmp(name, "glScissor") == 0) {
        stash_if_real((void**)&real_glScissor, real, (void*)glScissor);
        return (void*)glScissor;
    }
    if (strcmp(name, "glViewportIndexedf") == 0) {
        stash_if_real((void**)&real_glViewportIndexedf, real, (void*)glViewportIndexedf);
        return (void*)glViewportIndexedf;
    }
    if (strcmp(name, "glScissorIndexed") == 0) {
        stash_if_real((void**)&real_glScissorIndexed, real, (void*)glScissorIndexed);
        return (void*)glScissorIndexed;
    }
    if (strcmp(name, "glViewportArrayv") == 0) {
        stash_if_real((void**)&real_glViewportArrayv, real, (void*)glViewportArrayv);
        return (void*)glViewportArrayv;
    }
    if (strcmp(name, "glScissorArrayv") == 0) {
        stash_if_real((void**)&real_glScissorArrayv, real, (void*)glScissorArrayv);
        return (void*)glScissorArrayv;
    }
    return real;
}

static void ensure_dlsym(void) {
    if (real_dlsym) return;
    /* Do NOT call dlsym() here — we define it (same-DSO bind → recursion).
     * libc dlvsym is not interposed, so RTLD_NEXT finds the real dlsym. */
    DlvsymFn real_dlvsym = (DlvsymFn)dlvsym(RTLD_NEXT, "dlvsym", "GLIBC_2.34");
    if (!real_dlvsym) real_dlvsym = (DlvsymFn)dlvsym(RTLD_NEXT, "dlvsym", "GLIBC_2.2.5");
    if (real_dlvsym) {
        real_dlsym = (DlsymFn)real_dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym) real_dlsym = (DlsymFn)real_dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    }
}

XFSR_EXPORT void* dlsym(void* handle, const char* symbol) {
    ensure_dlsym();
    if (!real_dlsym) return NULL;
    if (strcmp(symbol, "dlsym") == 0) return (void*)dlsym;
    return wrap_proc(symbol, real_dlsym(handle, symbol));
}

static void* hook_get_proc(GetProcFn real, const char* name) {
    if (!real) return NULL;
    return wrap_proc(name, real(name));
}

XFSR_EXPORT void* glXGetProcAddress(const char* name) {
    static GetProcFn real = NULL;
    if (!real) real = (GetProcFn)resolve_next("glXGetProcAddress", (void*)glXGetProcAddress);
    return hook_get_proc(real, name);
}

XFSR_EXPORT void* glXGetProcAddressARB(const char* name) {
    static GetProcFn real = NULL;
    if (!real) {
        real = (GetProcFn)resolve_next("glXGetProcAddressARB", (void*)glXGetProcAddressARB);
        if (!real) real = (GetProcFn)resolve_next("glXGetProcAddress", (void*)glXGetProcAddress);
    }
    return hook_get_proc(real, name);
}

XFSR_EXPORT void* eglGetProcAddress(const char* name) {
    static GetProcFn real = NULL;
    if (!real) real = (GetProcFn)resolve_next("eglGetProcAddress", (void*)eglGetProcAddress);
    return hook_get_proc(real, name);
}

XFSR_EXPORT void* glfwGetProcAddress(const char* name) {
    static GetProcFn real = NULL;
    if (!real) real = (GetProcFn)resolve_next("glfwGetProcAddress", (void*)glfwGetProcAddress);
    return hook_get_proc(real, name);
}
