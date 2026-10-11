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
//               [--gles2 real|es3] [--expect-video sw|hw|black]
//               [--expect-pixel X,Y,RRGGBB]
//               [--sram-in FILE] [--sram-out FILE]
//
// --sram-in loads FILE into the core's SAVE_RAM in RetroArch's order: right
// after retro_load_game, before context_reset and the first retro_run, it
// reads retro_get_memory_size/data and copies min(file, size) bytes in.
// --sram-out writes SAVE_RAM to FILE after the last frame, as RetroArch does
// on unload (nothing is written if the size is 0).
// --state-out writes the state taken at --save-at to FILE; --state-in makes
// --restore-at restore FILE instead, so a state can cross sessions (save,
// quit, start again, load), which is when a JIT cart's slots are all gone.
// --unplug-at sets port 0 to RETRO_DEVICE_NONE at that frame.
// --gles2 plays a GLES2-only frontend (an old handheld such as a PowerVR
// SGX544): it refuses the GLES3 and GL core requests and grants only GLES2,
// with an EGL_CONTEXT_CLIENT_VERSION 2 context. "real" caps Mesa at ES 2.0
// (MESA_GLES_VERSION_OVERRIDE), so ES3 entry points and GLSL ES 3.00 are gone
// as on that GPU; "es3" keeps what Mesa returns for an ES2 request (ES 3.2),
// the driver that hands back more than it was asked for.
// --expect-video: the last frame was a software frame (sw), a hardware frame
// (hw), or nothing but black was ever shown (black); sw and hw also require pixels
// that are not all black in the last frame.
// --expect-pixel: the last frame (software or hardware) has that colour at
// X,Y, counted from the top-left of the cart's image (channel order check).
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
static const char* gles2 = NULL;   /* NULL, "real" or "es3" */
static int hw_granted = 0;
static unsigned long sw_frames = 0, hw_frames = 0;
static unsigned long sw_lit = 0;   /* non-black pixels in the last software frame */
static unsigned long long sw_rgb[3], hw_rgb[3];   /* channel sums of the last frame */
static long px_x = -1, px_y = -1;
static unsigned px_want = 0, px_got = 0xffffffffu;
static unsigned last_w = 0, last_h = 0;

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
        unsigned want = gles2 ? RETRO_HW_CONTEXT_OPENGLES2 : RETRO_HW_CONTEXT_OPENGLES3;
        if (cb->context_type != want) return false;
        hw_granted = 1;
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

static void video_cb(const void* d, unsigned w, unsigned h, size_t pitch) {
    if (!d) return;   /* dupe */
    last_w = w; last_h = h;
    if (d == RETRO_HW_FRAME_BUFFER_VALID) { hw_frames++; return; }
    sw_frames++;
    sw_lit = 0;
    sw_rgb[0] = sw_rgb[1] = sw_rgb[2] = 0;
    for (unsigned y = 0; y < h; y++) {
        const unsigned char* row = (const unsigned char*)d + y * pitch;
        for (unsigned x = 0; x < w; x++) {   /* XRGB8888: B, G, R, X in memory */
            if (row[x * 4] | row[x * 4 + 1] | row[x * 4 + 2]) sw_lit++;
            for (int c = 0; c < 3; c++) sw_rgb[c] += row[x * 4 + 2 - c];
            if ((long)x == px_x && (long)y == px_y)
                px_got = (unsigned)row[x * 4 + 2] << 16 | (unsigned)row[x * 4 + 1] << 8 | row[x * 4];
        }
    }
}
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
    const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, gles2 ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_ES3_BIT,
                                EGL_SURFACE_TYPE, 0, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) return 0;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, gles2 ? 2 : 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return 0;
    printf("lr_frontend: context %s\n", (const char*)glGetString(GL_VERSION));
    // The frontend's framebuffer: what RetroArch's hw_render FBO is. Built
    // with what ES 2.0 has (OES_rgb8_rgba8, OES_packed_depth_stencil: same
    // enum values as the ES3 ones), depth and stencil attached separately.
    GLuint rb[2];
    glGenFramebuffers(1, &fbo);
    glGenRenderbuffers(2, rb);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
    glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb[1]);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
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
    const char* expect_video = NULL;
    const char* sram_in = NULL;
    const char* sram_out = NULL;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--frames")) frames = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--save-at")) save_at = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--restore-at")) restore_at = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--expect-message")) expect_message = argv[i + 1];
        else if (!strcmp(argv[i], "--state-out")) state_out = argv[i + 1];
        else if (!strcmp(argv[i], "--state-in")) state_in = argv[i + 1];
        else if (!strcmp(argv[i], "--unplug-at")) unplug_at = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--gles2")) gles2 = argv[i + 1];
        else if (!strcmp(argv[i], "--sram-in")) sram_in = argv[i + 1];
        else if (!strcmp(argv[i], "--sram-out")) sram_out = argv[i + 1];
        else if (!strcmp(argv[i], "--expect-video")) expect_video = argv[i + 1];
        else if (!strcmp(argv[i], "--expect-pixel")) {
            if (sscanf(argv[i + 1], "%ld,%ld,%x", &px_x, &px_y, &px_want) != 3) return 2;
        }
        else if (!strcmp(argv[i], "--option") && opt_count < MAX_OPTIONS) {
            char* eq = strchr(argv[i + 1], '=');
            if (!eq) return 2;
            *eq = 0;
            opt_key[opt_count] = argv[i + 1];
            opt_val[opt_count++] = eq + 1;
        } else { fprintf(stderr, "lr_frontend: unknown argument %s\n", argv[i]); return 2; }
    }

    if (gles2 && strcmp(gles2, "real") && strcmp(gles2, "es3")) return 2;
    // Before the first context exists: Mesa reads it once.
    if (gles2 && !strcmp(gles2, "real")) setenv("MESA_GLES_VERSION_OVERRIDE", "2.0", 1);

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
    size_t (*retro_get_memory_size)(unsigned);
    void* (*retro_get_memory_data)(unsigned);
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_init); SYM(retro_load_game); SYM(retro_get_system_av_info); SYM(retro_run);
    SYM(retro_serialize_size); SYM(retro_serialize); SYM(retro_unserialize); SYM(retro_unload_game);
    SYM(retro_set_controller_port_device); SYM(retro_get_memory_size); SYM(retro_get_memory_data);

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    struct retro_game_info game = { argv[2], NULL, 0, NULL };
    if (!retro_load_game(&game)) { fprintf(stderr, "lr_frontend: load failed\n"); return 1; }
    if (sram_in) {
        // RetroArch's content_load_ram_file: before video init (context_reset)
        // and before the first retro_run; a size of 0 loads nothing.
        size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        void* data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        FILE* in = fopen(sram_in, "rb");
        if (!in) { fprintf(stderr, "lr_frontend: cannot read %s\n", sram_in); return 1; }
        unsigned char buf[65536];
        size_t n = fread(buf, 1, sizeof buf, in);
        fclose(in);
        if (n > size) n = size;
        if (data && n) memcpy(data, buf, n);
        printf("lr_frontend: SAVE_RAM size %zu at load, %zu bytes loaded\n", size, n);
    }
    struct retro_system_av_info av;
    memset(&av, 0, sizeof av);
    retro_get_system_av_info(&av);
    unsigned w = av.geometry.max_width ? av.geometry.max_width : 1920;
    unsigned h = av.geometry.max_height ? av.geometry.max_height : 1080;
    // A frontend that granted no hardware context still has a GL context of
    // its own (it draws its menu with it); the core must not touch it.
    if (!egl_setup(w, h)) { fprintf(stderr, "lr_frontend: no surfaceless EGL context\n"); return 1; }
    printf("lr_frontend: hardware context %s\n", hw_granted ? "granted" : "not requested or refused");
    if (hw_granted && hw.context_reset) hw.context_reset();

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
    if (sram_out) {
        size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
        void* data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
        FILE* o = size && data ? fopen(sram_out, "wb") : NULL;
        if (o) { fwrite(data, 1, size, o); fclose(o); }
        printf("lr_frontend: SAVE_RAM size %zu at unload%s\n", size, o ? ", written" : ", nothing written");
    }
    fflush(stdout);
    // _exit throughout: V8 teardown at exit is not what is under test.
    if (trapped) { printf("lr_frontend: FAIL: the cart trapped\n"); fflush(stdout); _exit(1); }
    unsigned long hw_lit = 0;
    if (hw_frames && last_w && last_h) {
        // What the core left in the frontend's framebuffer for the last frame.
        unsigned char* px = malloc((size_t)last_w * last_h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glReadPixels(0, 0, last_w, last_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        for (size_t i = 0; px && i < (size_t)last_w * last_h; i++) {
            if (px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2]) hw_lit++;
            for (int c = 0; c < 3; c++) hw_rgb[c] += px[i * 4 + c];
        }
        // bottom_left_origin: row 0 of the read-back is the image's bottom row
        if (px && px_x >= 0 && px_x < (long)last_w && px_y >= 0 && px_y < (long)last_h) {
            const unsigned char* q = px + ((size_t)(last_h - 1 - px_y) * last_w + px_x) * 4;
            px_got = (unsigned)q[0] << 16 | (unsigned)q[1] << 8 | q[2];
        }
        free(px);
    }
    printf("lr_frontend: video %lu software frames (last %ux%u, %lu lit, RGB sums %llu %llu %llu), "
           "%lu hardware frames (%lu lit, RGB sums %llu %llu %llu)\n",
           sw_frames, last_w, last_h, sw_lit, sw_rgb[0], sw_rgb[1], sw_rgb[2],
           hw_frames, hw_lit, hw_rgb[0], hw_rgb[1], hw_rgb[2]);
    if (px_x >= 0) {
        printf("lr_frontend: pixel %ld,%ld is %06x\n", px_x, px_y, px_got);
        if (px_got != px_want) {
            printf("lr_frontend: FAIL: expected %06x at %ld,%ld\n", px_want, px_x, px_y);
            fflush(stdout);
            _exit(1);
        }
    }
    if (expect_video) {
        int ok = !strcmp(expect_video, "sw")   ? sw_frames > 0 && sw_lit > 0 && hw_frames == 0
               : !strcmp(expect_video, "hw")   ? hw_frames > 0 && hw_lit > 0 && sw_frames == 0
               : !strcmp(expect_video, "black") ? sw_frames == 0 && hw_lit == 0
               : 0;
        if (!ok) {
            printf("lr_frontend: FAIL: expected video \"%s\"\n", expect_video);
            fflush(stdout);
            _exit(1);
        }
    }
    if (expect_message && !got_message) {
        printf("lr_frontend: FAIL: no on-screen message containing \"%s\"\n", expect_message);
        fflush(stdout);
        _exit(1);
    }
    printf("lr_frontend: PASS (%ld frames, %lu audio frames)\n", frames, audio_frames);
    fflush(stdout);
    _exit(0);
}
