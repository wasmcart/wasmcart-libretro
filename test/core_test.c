/*
 * core_test -- a minimal libretro frontend for the built core.
 *
 * Loads wasmcart_libretro.so the way RetroArch does (dlopen + retro_*), hands
 * it a GLES3 hardware-render context on Mesa's surfaceless EGL display (no
 * window, no X/Wayland), runs a cart for some frames, and prints what the
 * cart left in its save region (RETRO_MEMORY_SAVE_RAM, a live view of cart
 * memory). test/run.sh asserts on that line and on the log.
 *
 *   core_test <core.so> <cart.wasm|.wasc> [frames]
 *
 * Output: the core's log lines, then
 *   RESULT magic=0x... frames=N ok=N allocs=N frees=N trapped=0|1
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include "libretro.h"

#define W 640
#define H 480

static struct retro_hw_render_callback* hw;
static GLuint fbo;
static bool trapped;

static void log_printf(enum retro_log_level level, const char* fmt, ...) {
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "cart trapped")) trapped = true;
    fprintf(stdout, "[core %d] %s", (int)level, line);
    fflush(stdout);
}

static uintptr_t get_fb(void) { return fbo; }
static retro_proc_address_t get_proc(const char* sym) { return (retro_proc_address_t)eglGetProcAddress(sym); }

static bool environ_cb(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback*)data)->log = log_printf;
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        struct retro_hw_render_callback* cb = data;
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3) return false;
        cb->get_current_framebuffer = get_fb;
        cb->get_proc_address = get_proc;
        hw = cb;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void* d, unsigned w, unsigned h, size_t p) { (void)d; (void)w; (void)h; (void)p; }
static size_t audio_cb(const int16_t* d, size_t n) { (void)d; return n; }
static void poll_cb(void) {}
static int16_t input_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) { (void)port; (void)dev; (void)idx; (void)id; return 0; }

static bool make_context(void) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!get_display) return false;
    EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) return false;
    EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) return false;
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return false;
    GLuint color, ds;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &ds);
    glBindRenderbuffer(GL_RENDERBUFFER, ds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

#define SYM(name) do { *(void**)&name = dlsym(core, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 2; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <core.so> <cart> [frames]\n", argv[0]); return 2; }
    int frames = argc > 3 ? atoi(argv[3]) : 10;
    if (!make_context()) { fprintf(stderr, "core_test: no surfaceless EGL GLES3 context\n"); return 2; }

    void* core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!core) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void);
    bool (*retro_load_game)(const struct retro_game_info*);
    void (*retro_run)(void);
    void* (*retro_get_memory_data)(unsigned);
    size_t (*retro_get_memory_size)(unsigned);
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_init);
    SYM(retro_load_game); SYM(retro_run); SYM(retro_get_memory_data); SYM(retro_get_memory_size);

    retro_set_environment(environ_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample_batch(audio_cb);
    retro_set_input_poll(poll_cb);
    retro_set_input_state(input_cb);
    retro_init();
    struct retro_game_info game = { .path = argv[2] };
    if (!retro_load_game(&game)) { printf("RESULT load_failed\n"); return 1; }
    if (hw && hw->context_reset) hw->context_reset();
    for (int i = 0; i < frames; i++) retro_run();

    uint32_t s[5] = {0};
    void* save = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (save && retro_get_memory_size(RETRO_MEMORY_SAVE_RAM) >= sizeof s) memcpy(s, save, sizeof s);
    printf("RESULT magic=0x%08X frames=%u ok=%u allocs=%u frees=%u trapped=%d\n",
           s[0], s[1], s[2], s[3], s[4], trapped ? 1 : 0);
    fflush(stdout);
    /* retro_deinit tears V8 down; a test does not need it, and skipping it
       keeps a cart that stopped mid-call from being re-entered at exit. */
    _exit(0);
}
