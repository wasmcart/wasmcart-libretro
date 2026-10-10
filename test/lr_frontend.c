// lr_frontend.c — a minimal libretro frontend for the core's regression tests.
//
// Loads the core, gives it a headless GLES3 context (Mesa's surfaceless EGL
// display, never the user's X or Wayland server), runs a cart for a number of
// frames and can take a save state and restore it mid-run, the way RetroArch
// does for save states, rewind and run-ahead.
//
//   lr_frontend <core.so> <cart.wasc> [--frames N] [--save-at F] [--restore-at F]
//               [--option key=value]... [--expect-message TEXT]
//               [--state-out FILE] [--state-in FILE] [--unplug-at F]
//
// --state-out writes the state taken at --save-at to FILE; --state-in makes
// --restore-at restore FILE instead, so a state can cross sessions (save,
// quit, start again, load), which is when a JIT cart's slots are all gone.
// --unplug-at sets port 0 to RETRO_DEVICE_NONE at that frame.
//
// Exit status: 0 pass; 1 the cart trapped, an expected on-screen message never
// came, or a call failed; a crash (the bug these tests exist for) is a signal.
#include "libretro.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_OPTIONS 8
static const char* opt_key[MAX_OPTIONS];
static const char* opt_val[MAX_OPTIONS];
static int opt_count = 0;

static const char* expect_message = NULL;
static int got_message = 0;
static int trapped = 0;
static unsigned long audio_frames = 0;

static struct retro_hw_render_callback hw;
static GLuint fbo = 0;

static void note_message(const char* msg) {
    printf("lr_frontend: MESSAGE: %s\n", msg);
    if (expect_message && strstr(msg, expect_message)) got_message = 1;
}

static void log_cb(enum retro_log_level level, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strstr(buf, "trapped")) trapped = 1;
    if (level >= RETRO_LOG_WARN || getenv("LR_VERBOSE")) fputs(buf, stdout);
}

static uintptr_t get_fb(void) { return fbo; }
static retro_proc_address_t get_proc(const char* sym) { return (retro_proc_address_t)eglGetProcAddress(sym); }

static bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback*)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable* v = data;
        for (int i = 0; i < opt_count; i++)
            if (strcmp(v->key, opt_key[i]) == 0) { v->value = opt_val[i]; return true; }
        v->value = NULL;
        return false;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        struct retro_hw_render_callback* cb = data;
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGLES3) return false;
        cb->get_current_framebuffer = get_fb;
        cb->get_proc_address = get_proc;
        hw = *cb;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned*)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
        note_message(((const struct retro_message_ext*)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
        note_message(((const struct retro_message*)data)->msg);
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void* d, unsigned w, unsigned h, size_t pitch) { (void)d; (void)w; (void)h; (void)pitch; }
static size_t audio_batch_cb(const int16_t* d, size_t frames) { (void)d; audio_frames += frames; return frames; }
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)port; (void)dev; (void)idx; (void)id;
    return 0;
}

static int egl_setup(unsigned w, unsigned h) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                                 : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) return 0;
    static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) return 0;
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 0;
    // The frontend's framebuffer: what RetroArch's hw_render FBO is.
    GLuint rb[2];
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(2, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

#define SYM(name) do { *(void**)&name = dlsym(core, #name); if (!name) { fprintf(stderr, "lr_frontend: no %s\n", #name); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <core.so> <cart.wasc> [--frames N] [--save-at F] [--restore-at F] "
                        "[--option key=value]... [--expect-message TEXT]\n", argv[0]);
        return 2;
    }
    long frames = 300, save_at = -1, restore_at = -1;
    const char* state_out = NULL;
    const char* state_in = NULL;
    long unplug_at = -1;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--frames")) frames = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--save-at")) save_at = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--restore-at")) restore_at = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--expect-message")) expect_message = argv[i + 1];
        else if (!strcmp(argv[i], "--state-out")) state_out = argv[i + 1];
        else if (!strcmp(argv[i], "--state-in")) state_in = argv[i + 1];
        else if (!strcmp(argv[i], "--unplug-at")) unplug_at = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--option") && opt_count < MAX_OPTIONS) {
            char* eq = strchr(argv[i + 1], '=');
            if (!eq) return 2;
            *eq = 0;
            opt_key[opt_count] = argv[i + 1];
            opt_val[opt_count++] = eq + 1;
        } else { fprintf(stderr, "lr_frontend: unknown argument %s\n", argv[i]); return 2; }
    }

    void* core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!core) { fprintf(stderr, "lr_frontend: %s\n", dlerror()); return 1; }
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void);
    bool (*retro_load_game)(const struct retro_game_info*);
    void (*retro_get_system_av_info)(struct retro_system_av_info*);
    void (*retro_run)(void);
    size_t (*retro_serialize_size)(void);
    bool (*retro_serialize)(void*, size_t);
    bool (*retro_unserialize)(const void*, size_t);
    void (*retro_unload_game)(void);
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_init); SYM(retro_load_game); SYM(retro_get_system_av_info); SYM(retro_run);
    SYM(retro_serialize_size); SYM(retro_serialize); SYM(retro_unserialize); SYM(retro_unload_game);
    SYM(retro_set_controller_port_device);

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    struct retro_game_info game = { argv[2], NULL, 0, NULL };
    if (!retro_load_game(&game)) { fprintf(stderr, "lr_frontend: load failed\n"); return 1; }
    struct retro_system_av_info av;
    memset(&av, 0, sizeof av);
    retro_get_system_av_info(&av);
    unsigned w = av.geometry.max_width ? av.geometry.max_width : 1920;
    unsigned h = av.geometry.max_height ? av.geometry.max_height : 1080;
    if (!egl_setup(w, h)) { fprintf(stderr, "lr_frontend: no surfaceless EGL GLES3 context\n"); return 1; }
    if (hw.context_reset) hw.context_reset();

    void* state = NULL;
    size_t state_size = 0;
    for (long f = 0; f < frames && !trapped; f++) {
        if (f == save_at) {
            state_size = retro_serialize_size();
            state = malloc(state_size);
            if (!state || !retro_serialize(state, state_size)) { fprintf(stderr, "lr_frontend: serialize failed\n"); return 1; }
            printf("lr_frontend: frame %ld saved %zu bytes\n", f, state_size);
            FILE* o = state_out ? fopen(state_out, "wb") : NULL;
            if (state_out && (!o || fwrite(state, 1, state_size, o) != state_size)) { fprintf(stderr, "lr_frontend: cannot write %s\n", state_out); return 1; }
            if (o) fclose(o);
        }
        if (f == restore_at && state_in) {
            FILE* in = fopen(state_in, "rb");
            if (!in) { fprintf(stderr, "lr_frontend: cannot read %s\n", state_in); return 1; }
            fseek(in, 0, SEEK_END);
            state_size = (size_t)ftell(in);
            fseek(in, 0, SEEK_SET);
            free(state);
            state = malloc(state_size);
            if (!state || fread(state, 1, state_size, in) != state_size) { fprintf(stderr, "lr_frontend: cannot read %s\n", state_in); return 1; }
            fclose(in);
        }
        if (f == restore_at) {
            if (!state || !retro_unserialize(state, state_size)) { fprintf(stderr, "lr_frontend: unserialize failed\n"); return 1; }
            printf("lr_frontend: frame %ld restored\n", f);
        }
        if (f == unplug_at) {
            retro_set_controller_port_device(0, RETRO_DEVICE_NONE);
            printf("lr_frontend: frame %ld port 0 unplugged\n", f);
        }
        retro_run();
    }
    fflush(stdout);
    // _exit throughout: V8 teardown at exit is not what is under test.
    if (trapped) { printf("lr_frontend: FAIL: the cart trapped\n"); fflush(stdout); _exit(1); }
    if (expect_message && !got_message) {
        printf("lr_frontend: FAIL: no on-screen message containing \"%s\"\n", expect_message);
        fflush(stdout);
        _exit(1);
    }
    printf("lr_frontend: PASS (%ld frames, %lu audio frames)\n", frames, audio_frames);
    fflush(stdout);
    _exit(0);
}
