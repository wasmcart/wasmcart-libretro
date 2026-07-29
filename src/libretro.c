// libretro.c — wasmcart libretro core
// Bridges RetroArch ↔ wasmcart host API (cart_host.cpp)

#include "libretro.h"
#include "../include/wasmcart_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <GLES3/gl3.h>
// Redirect this file's glXxx() (cart GL-state save/restore) to pointers loaded
// from RetroArch's get_proc_address — the core links no GL library of its own.
#include "gl_procs.h"
#include <wc_log.h>

// ─── Libretro callbacks ─────────────────────────────────────────────────────

static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_log_printf_t log_cb;

// ─── Core state ─────────────────────────────────────────────────────────────

static wc_host_t* host = NULL;
static bool uses_gl = false;
static bool gl_context_ready = false;
static bool first_frame = true;
static uint32_t cart_w = 0, cart_h = 0;
static uint32_t frame_count = 0;
static double time_ms = 0;

// Per-frame delta the frontend reports via RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK
// (called right before each retro_run). 0 until the first callback (then we fall
// back to a fixed 1000/60 step). Carts read wc_time.delta_ms for motion (e.g.
// racer's physics/AI/camera/audio all scale by dt), so a blind fixed step makes a
// genuine frontend stall produce SLOW-MOTION instead of a dropped frame. The
// frontend substitutes the reference value during fast-forward / slow-motion /
// frame-stepping / pause, so those stay correct without the core detecting them.
static double frame_time_ms = 0.0;
static void frame_time_cb(retro_usec_t usec) { frame_time_ms = (double)usec / 1000.0; }

// Audio conversion buffer (F32 → S16)
static int16_t* audio_conv_buf = NULL;
static uint32_t audio_conv_cap = 0;

// ─── GL state save/restore ──────────────────────────────────────────────────
// Save cart's GL state after each frame, restore before next frame.
// RetroArch modifies GL state between retro_run calls (overlay/menu).
// Without save/restore, carts with internal GL state caches (Ganesh, gl4es,
// chromium_bsu's gl_compat) see stale state and render nothing.

static struct {
    GLint program;
    GLint vao;
    GLint fbo;
    GLint active_tex;
    GLint tex_2d;
    GLint array_buf;
    GLint elem_buf;
    GLint viewport[4];
    GLboolean blend;
    GLboolean depth_test;
    GLboolean scissor_test;
    GLboolean stencil_test;
    GLboolean cull_face;
    GLboolean dither;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint blend_eq_rgb, blend_eq_a;
    GLboolean depth_mask;
    GLboolean color_mask[4];
    GLint stencil_mask;
    GLfloat clear_color[4];
    bool saved;
} cart_gl_state = {0};

static void save_cart_gl_state(void) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &cart_gl_state.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &cart_gl_state.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cart_gl_state.fbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &cart_gl_state.active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &cart_gl_state.tex_2d);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &cart_gl_state.array_buf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &cart_gl_state.elem_buf);
    glGetIntegerv(GL_VIEWPORT, cart_gl_state.viewport);
    cart_gl_state.blend = glIsEnabled(GL_BLEND);
    cart_gl_state.depth_test = glIsEnabled(GL_DEPTH_TEST);
    cart_gl_state.scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    cart_gl_state.stencil_test = glIsEnabled(GL_STENCIL_TEST);
    cart_gl_state.cull_face = glIsEnabled(GL_CULL_FACE);
    cart_gl_state.dither = glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_BLEND_SRC_RGB, &cart_gl_state.blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &cart_gl_state.blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &cart_gl_state.blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &cart_gl_state.blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &cart_gl_state.blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &cart_gl_state.blend_eq_a);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &cart_gl_state.depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, cart_gl_state.color_mask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &cart_gl_state.stencil_mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cart_gl_state.clear_color);
    cart_gl_state.saved = true;
}

static void restore_cart_gl_state(void) {
    if (!cart_gl_state.saved) return;
    glUseProgram(cart_gl_state.program);
    glBindVertexArray(cart_gl_state.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, cart_gl_state.fbo);
    glActiveTexture(cart_gl_state.active_tex);
    glBindTexture(GL_TEXTURE_2D, cart_gl_state.tex_2d);
    glBindBuffer(GL_ARRAY_BUFFER, cart_gl_state.array_buf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cart_gl_state.elem_buf);
    glViewport(cart_gl_state.viewport[0], cart_gl_state.viewport[1],
               cart_gl_state.viewport[2], cart_gl_state.viewport[3]);
    if (cart_gl_state.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (cart_gl_state.depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cart_gl_state.scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (cart_gl_state.stencil_test) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (cart_gl_state.cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (cart_gl_state.dither) glEnable(GL_DITHER); else glDisable(GL_DITHER);
    glBlendFuncSeparate(cart_gl_state.blend_src_rgb, cart_gl_state.blend_dst_rgb,
                        cart_gl_state.blend_src_a, cart_gl_state.blend_dst_a);
    glBlendEquationSeparate(cart_gl_state.blend_eq_rgb, cart_gl_state.blend_eq_a);
    glDepthMask(cart_gl_state.depth_mask);
    glColorMask(cart_gl_state.color_mask[0], cart_gl_state.color_mask[1],
                cart_gl_state.color_mask[2], cart_gl_state.color_mask[3]);
    glStencilMask(cart_gl_state.stencil_mask);
    glClearColor(cart_gl_state.clear_color[0], cart_gl_state.clear_color[1],
                 cart_gl_state.clear_color[2], cart_gl_state.clear_color[3]);
}

static void reset_gl_for_retroarch(void) {
    // Minimal reset — only state RetroArch's overlay renderer needs.
    // Don't touch textures, samplers, buffers — carts cache those.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    glActiveTexture(GL_TEXTURE0);
}

// ─── Callbacks ──────────────────────────────────────────────────────────────

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

// ─── Core options ───────────────────────────────────────────────────────────

static uint32_t pref_width = 1920;
static uint32_t pref_height = 1080;

static const struct retro_core_option_v2_definition option_defs[] = {
    {
        "wasmcart_resolution",
        "Internal Resolution",
        NULL,
        "Resolution passed to the cart. The cart decides its actual render size.",
        NULL, "video",
        {
            { "640x480",   "640x480" },
            { "1280x720",  "1280x720 (720p)" },
            { "1920x1080", "1920x1080 (1080p)" },
            { "2560x1440", "2560x1440 (1440p)" },
            { "3840x2160", "3840x2160 (4K)" },
            { NULL, NULL },
        },
        "1920x1080"
    },
    { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL },
};

static const struct retro_core_options_v2 options_v2 = {
    NULL,  // no categories
    option_defs,
};

static void check_options(void) {
    struct retro_variable var = { "wasmcart_resolution", NULL };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        unsigned w = 0, h = 0;
        if (sscanf(var.value, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
            pref_width = w;
            pref_height = h;
        }
    }
}

// ─── Rumble (ABI v3) ───────────────────────────────────────────────────────
//
// The cart drives rumble, so wasmcart-native asks US to actuate it. libretro
// gives us set_rumble_state(port, effect, strength) with NO duration, so the
// duration the cart asked for has to be run down on our own frame timer and
// the motors stopped explicitly -- otherwise a one-shot effect buzzes forever.
//
// The two models line up cleanly: the ABI's low/high are the strong/weak
// motors, which is exactly RETRO_RUMBLE_STRONG / RETRO_RUMBLE_WEAK.
static struct retro_rumble_interface rumble_iface;
static bool rumble_available = false;

// Frames remaining per pad, decremented in retro_run at the fixed 60fps step.
static int rumble_frames_left[WC_MAX_PADS];

static int lr_has_rumble(void* user, uint32_t pad_id) {
    (void)user;
    if (!rumble_available || pad_id >= WC_MAX_PADS) return 0;
    // Per-DEVICE, not per-platform. libretro has no "can this pad rumble"
    // query, so probe by setting zero strength: a port with no rumble returns
    // false and one with rumble returns true, without the player feeling it.
    return rumble_iface.set_rumble_state(pad_id, RETRO_RUMBLE_STRONG, 0) ? 1 : 0;
}

static void lr_rumble(void* user, uint32_t pad_id, float low, float high,
                      uint32_t duration_ms) {
    (void)user;
    if (!rumble_available || pad_id >= WC_MAX_PADS) return;
    // low/high arrive already clamped to 0..1 and duration already capped at
    // WC_RUMBLE_MAX_MS -- wasmcart-native does that so every backend agrees.
    // 0xFFFF, not 0xFFFE: libretro's strength is a full uint16 range.
    uint16_t strong = (uint16_t)(low  * 65535.0f + 0.5f);
    uint16_t weak   = (uint16_t)(high * 65535.0f + 0.5f);
    rumble_iface.set_rumble_state(pad_id, RETRO_RUMBLE_STRONG, strong);
    rumble_iface.set_rumble_state(pad_id, RETRO_RUMBLE_WEAK,   weak);
    // Round UP so a sub-frame effect still lasts one visible frame rather than
    // being cancelled before the player feels anything.
    rumble_frames_left[pad_id] = (int)((duration_ms * 60 + 999) / 1000);
    if (rumble_frames_left[pad_id] < 1) rumble_frames_left[pad_id] = 1;
}

static void lr_rumble_stop(void* user, uint32_t pad_id) {
    (void)user;
    if (!rumble_available || pad_id >= WC_MAX_PADS) return;
    rumble_iface.set_rumble_state(pad_id, RETRO_RUMBLE_STRONG, 0);
    rumble_iface.set_rumble_state(pad_id, RETRO_RUMBLE_WEAK,   0);
    rumble_frames_left[pad_id] = 0;
}

// Run the duration timers down; called once per frame from retro_run.
static void rumble_tick(void) {
    if (!rumble_available) return;
    for (uint32_t i = 0; i < WC_MAX_PADS; i++) {
        if (rumble_frames_left[i] > 0 && --rumble_frames_left[i] == 0)
            lr_rumble_stop(NULL, i);
    }
}

void retro_set_environment(retro_environment_t cb) {
    environ_cb = cb;

    // Set up logging
    struct retro_log_callback logging;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;

    // Core options
    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, (void*)&options_v2);

    // We support no-game = false (need a .wasc to load)
    bool no_game = false;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

// ─── Core info ──────────────────────────────────────────────────────────────

void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "wasmcart";
    info->library_version = "0.1.0";
    info->valid_extensions = "wasc|wasm";
    info->need_fullpath = true;   // we read the file ourselves (ZIP)
    info->block_extract = true;   // don't extract, we handle ZIP
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = cart_w;
    info->geometry.base_height = cart_h;
    info->geometry.max_width = pref_width;
    info->geometry.max_height = pref_height;
    info->geometry.aspect_ratio = (float)cart_w / (float)cart_h;
    info->timing.fps = 60.0;

    const wc_cart_info_t* ci = wc_host_get_cart_info(host);
    info->timing.sample_rate = ci->audio_sample_rate ? (double)ci->audio_sample_rate : 48000.0;
}

// ─── HW render (GL carts) ───────────────────────────────────────────────────

static struct retro_hw_render_callback hw_render;  // persists — RetroArch fills callbacks

static void on_context_reset(void) {
    if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: GL context reset\n");
    gl_context_ready = true;

    // Get RetroArch's hw render callbacks
    struct retro_hw_render_callback* hw_cb = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &hw_cb)) {
        // Not needed for basic operation
    }

    // Now GL is available — finish deferred init
    if (host) {
        // RetroArch is the host: give wasmcart its GL via the frontend's
        // get_proc_address (the common libretro GL context — GLES3 on Android /
        // handhelds, whatever the desktop frontend provides). The shared GL layer
        // loads its entry points from this, so the core links NO GL library of
        // its own (no ANGLE, single self-contained file).
        wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)hw_render.get_proc_address);

        // Create redirect FBO with depth+stencil BEFORE cart init.
        // Three.js needs depth testing, Ganesh needs stencil.
        // RetroArch's hw_render FBO may not have these attachments.
        extern void wc_gl_setup_redirect(uint32_t w, uint32_t h);
        wc_gl_setup_redirect(pref_width, pref_height);

        wc_host_finish_init(host);

        // Re-read dimensions (may have changed after init)
        const wc_cart_info_t* ci = wc_host_get_cart_info(host);
        cart_w = ci->width;
        cart_h = ci->height;

        // Use cart's actual resolution (may differ from pref if cart overrides)
        uint32_t redir_w = cart_w > pref_width ? cart_w : pref_width;
        uint32_t redir_h = cart_h > pref_height ? cart_h : pref_height;
        wc_gl_setup_redirect(redir_w, redir_h);

        // Update full AV info — SET_GEOMETRY alone won't resize RetroArch's FBO
        struct retro_system_av_info av = {0};
        av.geometry.base_width = redir_w;
        av.geometry.base_height = redir_h;
        av.geometry.max_width = redir_w;
        av.geometry.max_height = redir_h;
        av.geometry.aspect_ratio = (float)redir_w / (float)redir_h;
        av.timing.fps = 60.0;
        const wc_cart_info_t* ci2 = wc_host_get_cart_info(host);
        av.timing.sample_rate = ci2->audio_sample_rate ? (double)ci2->audio_sample_rate : 48000.0;
        environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
        wc_log("wasmcart: SET_SYSTEM_AV_INFO %ux%u\n", redir_w, redir_h);

        // Save cart's initial GL state so restore works on first frame
        save_cart_gl_state();
    }
}

static void on_context_destroy(void) {
    if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: GL context destroyed\n");
}

// ─── Init / Deinit ──────────────────────────────────────────────────────────

void retro_init(void) {
    host = wc_host_create();
    if (!host && log_cb)
        log_cb(RETRO_LOG_ERROR, "wasmcart: failed to create host\n");

    // Rumble is optional: a frontend without it simply says no, and the cart's
    // wc_pad_has_rumble then reports 0 rather than the cart failing to load.
    rumble_available = environ_cb &&
        environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble_iface);
    memset(rumble_frames_left, 0, sizeof(rumble_frames_left));
    if (host && rumble_available) {
        wc_rumble_backend_t backend = {
            .has_rumble = lr_has_rumble,
            .rumble     = lr_rumble,
            .stop       = lr_rumble_stop,
            .user       = NULL,
        };
        wc_host_set_rumble_backend(host, &backend);
    }
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "wasmcart: rumble %s\n",
               rumble_available ? "available" : "not supported by frontend");
}

void retro_deinit(void) {
    for (uint32_t i = 0; i < WC_MAX_PADS; i++) lr_rumble_stop(NULL, i);
    wc_host_exit_v8();
    wc_host_destroy(host);
    host = NULL;
    free(audio_conv_buf);
    audio_conv_buf = NULL;
    audio_conv_cap = 0;
}

// ─── Load game ──────────────────────────────────────────────────────────────

bool retro_load_game(const struct retro_game_info* game) {
    if (!host || !game || !game->path) return false;

    // Set up file logging next to the cart (Android: logcat buffer too small)
    {
        const char* slash = strrchr(game->path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - game->path);
            char log_path[1024];
            snprintf(log_path, sizeof(log_path), "%.*s/wasmcart.log", (int)dir_len, game->path);
            wc_log_set_file(log_path);
        }
    }

    // Set pixel format for 2D carts
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    // Ask the frontend to report per-frame elapsed time so carts get a real dt
    // (idiomatic libretro timing). reference = ideal µs/frame; the frontend
    // substitutes it during FF/slow-mo/pause. Optional — if the frontend declines,
    // delta falls back to a fixed 1000/60 below (exact current behavior).
    struct retro_frame_time_callback frame_time = {
        frame_time_cb, (retro_usec_t)(1000000.0 / 60.0)
    };
    environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &frame_time);

    // Load the .wasc
    // For GL carts, defer _initialize/wc_init until GL context is ready
    wc_host_options_t opts = {0};
    opts.host_fps = 60;
    opts.audio_sample_rate = 48000;
    check_options();
    opts.preferred_width = pref_width;
    opts.preferred_height = pref_height;
    opts.defer_init = true;  // always defer — we'll finish in context_reset or first frame

    int rc = wc_host_load_file(host, game->path, &opts);
    if (rc != 0) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "wasmcart: failed to load %s\n", game->path);
        return false;
    }

    // Hold V8 locker persistently — RetroArch calls retro_run from the same thread
    wc_host_enter_v8();

    const wc_cart_info_t* info = wc_host_get_cart_info(host);
    cart_w = info->width;
    cart_h = info->height;
    uses_gl = wc_host_uses_gl(host);

    const wc_manifest_t* manifest = wc_host_get_manifest(host);
    if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: loaded %s (%ux%u, %s)\n",
        manifest->name, cart_w, cart_h, uses_gl ? "GL" : "2D");

    // Always request HW render — unified GL display path for all carts.
    // 2D carts get their framebuffer uploaded to the redirect FBO as a texture.
    memset(&hw_render, 0, sizeof(hw_render));
    hw_render.context_reset = on_context_reset;
    hw_render.context_destroy = on_context_destroy;
    hw_render.bottom_left_origin = true;
    hw_render.depth = true;
    hw_render.stencil = true;

    bool got_context = false;
    hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES3;
    hw_render.version_major = 3;
    hw_render.version_minor = 0;
    if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
        got_context = true;
    } else {
        hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
        hw_render.version_major = 3;
        hw_render.version_minor = 3;
        if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render)) {
            got_context = true;
        } else {
            hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
            hw_render.version_major = 2;
            hw_render.version_minor = 0;
            got_context = environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
        }
    }
    if (!got_context) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "wasmcart: failed to get HW render context\n");
        return false;
    }

    frame_count = 0;
    time_ms = 0;

    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num) {
    (void)type; (void)info; (void)num;
    return false;
}

void retro_unload_game(void) {
    // Save data is handled via retro_get_memory_*
}

// ─── Run frame ──────────────────────────────────────────────────────────────

void retro_run(void) {
    if (!host || wc_host_has_trapped(host)) return;

    // Safety fallback: finish init if context_reset wasn't called
    if (first_frame) {
        first_frame = false;
        if (!gl_context_ready) {
            wc_host_finish_init(host);
            const wc_cart_info_t* ci = wc_host_get_cart_info(host);
            cart_w = ci->width;
            cart_h = ci->height;
        }
    }

    // Run rumble durations down before polling, so an effect started last
    // frame stops on time even if the cart does not call rumble again.
    rumble_tick();

    // 1. Poll input
    input_poll_cb();

    // 2. Translate RetroArch input → wasmcart pads
    wc_pad_t pads[WC_MAX_PADS];
    memset(pads, 0, sizeof(pads));

    for (int p = 0; p < WC_MAX_PADS; p++) {
        uint16_t buttons = 0;
        // RetroArch uses SNES layout: B=south, A=east, Y=west, X=north
        // wasmcart uses Xbox/W3C layout: A=south, B=east, X=west, Y=north
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))     buttons |= WC_BUTTON_A;  // south
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))     buttons |= WC_BUTTON_B;  // east
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))     buttons |= WC_BUTTON_X;  // west
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X))     buttons |= WC_BUTTON_Y;  // north
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))     buttons |= WC_BUTTON_L;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))     buttons |= WC_BUTTON_R;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)) buttons |= WC_BUTTON_START;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT)) buttons |= WC_BUTTON_SELECT;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))    buttons |= WC_BUTTON_UP;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))  buttons |= WC_BUTTON_DOWN;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))  buttons |= WC_BUTTON_LEFT;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) buttons |= WC_BUTTON_RIGHT;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3))    buttons |= WC_BUTTON_L3;
        if (input_state_cb(p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3))    buttons |= WC_BUTTON_R3;

        pads[p].buttons = buttons;
        pads[p].left_x = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
        pads[p].left_y = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        pads[p].right_x = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
        pads[p].right_y = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
        pads[p].left_trigger = (uint8_t)(input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2) >> 8);
        pads[p].right_trigger = (uint8_t)(input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2) >> 8);
        pads[p].connected = (p == 0) ? 1 : 0; // Port 0 always connected
    }
    wc_host_set_pads(host, pads);

    // 3. Set time — use the frontend-reported per-frame delta when available so a
    // slow retro_run yields a correct large dt (carts drop a frame) instead of
    // slow-motion; the frontend feeds the reference during FF/slow-mo/pause so
    // those stay correct. Clamp to 4 frames so a long hitch/resume can't teleport
    // a dt-correct cart. Falls back to a fixed 1000/60 if the frontend never
    // registers the callback (frame_time_ms == 0) — exact prior behavior.
    const double FIXED_STEP = 1000.0 / 60.0;
    double delta_ms = (frame_time_ms > 0.0)
        ? (frame_time_ms < FIXED_STEP * 4.0 ? frame_time_ms : FIXED_STEP * 4.0)
        : FIXED_STEP;
    time_ms += delta_ms;
    wc_host_set_time(host, time_ms, delta_ms, frame_count);
    frame_count++;

    // 4. Run one frame — restore cart's GL state before rendering
    if (gl_context_ready) {
        restore_cart_gl_state();
        extern void wc_gl_rebind_redirect(void);
        wc_gl_rebind_redirect();
    }
    wc_host_run_frame(host);

    if (wc_host_has_trapped(host)) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "wasmcart: cart trapped\n");
        return;
    }

    // 5. Present video — unified GL path for all carts
    if (hw_render.get_current_framebuffer) {
        uintptr_t ra_fbo = hw_render.get_current_framebuffer();

        // For 2D carts: upload framebuffer pixels to redirect FBO as texture
        if (!uses_gl) {
            uint32_t fb_w, fb_h;
            const uint8_t* fb = wc_host_get_framebuffer(host, &fb_w, &fb_h);
            if (fb && fb_w > 0 && fb_h > 0) {
                extern void wc_gl_upload_framebuffer(const uint8_t* pixels, uint32_t w, uint32_t h);
                wc_gl_upload_framebuffer(fb, fb_w, fb_h);
            }
        }

        extern int wc_gl_has_redirect(void);
        extern void wc_gl_get_blit_size(uint32_t* w, uint32_t* h);
        uint32_t blit_w = 0, blit_h = 0;
        wc_gl_get_blit_size(&blit_w, &blit_h);
        if (!blit_w) blit_w = cart_w > pref_width ? cart_w : pref_width;
        if (!blit_h) blit_h = cart_h > pref_height ? cart_h : pref_height;

        if (wc_gl_has_redirect()) {
            extern void wc_gl_blit_to_fbo(uint32_t target_fbo, uint32_t cart_w, uint32_t cart_h, uint32_t dst_w, uint32_t dst_h, int flip_y);
            wc_gl_blit_to_fbo((uint32_t)ra_fbo, blit_w, blit_h, blit_w, blit_h, 0);
        }

        // Save cart's GL state, then reset for RetroArch overlay/menu
        save_cart_gl_state();
        reset_gl_for_retroarch();

        video_cb(RETRO_HW_FRAME_BUFFER_VALID, blit_w, blit_h, 0);
    }

    // 6. Send audio
    uint32_t num_frames;
    bool is_f32;
    const void* audio = wc_host_get_audio(host, &num_frames, &is_f32);
    if (num_frames > 0 && audio_batch_cb) {
        if (is_f32) {
            // Convert F32 → S16 (libretro only accepts int16)
            uint32_t needed = num_frames * 2; // stereo
            if (audio_conv_cap < needed) {
                audio_conv_buf = (int16_t*)realloc(audio_conv_buf, needed * sizeof(int16_t));
                audio_conv_cap = needed;
            }
            const float* src = (const float*)audio;
            for (uint32_t i = 0; i < needed; i++) {
                float s = src[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                audio_conv_buf[i] = (int16_t)(s * 32767.0f);
            }
            audio_batch_cb(audio_conv_buf, num_frames);
        } else {
            audio_batch_cb((const int16_t*)audio, num_frames);
        }
    }
}

// ─── Save states ────────────────────────────────────────────────────────────

size_t retro_serialize_size(void) {
    uint32_t size;
    wc_host_get_memory(host, &size);
    return size;
}

bool retro_serialize(void* data, size_t size) {
    uint32_t mem_size;
    void* mem = wc_host_get_memory(host, &mem_size);
    if (!mem || size < mem_size) return false;
    memcpy(data, mem, mem_size);
    return true;
}

bool retro_unserialize(const void* data, size_t size) {
    uint32_t mem_size;
    void* mem = wc_host_get_memory(host, &mem_size);
    if (!mem || size < mem_size) return false;
    memcpy(mem, data, mem_size);
    return true;
}

// ─── Persistent save (SRAM) ─────────────────────────────────────────────────

size_t retro_get_memory_size(unsigned id) {
    if (id != RETRO_MEMORY_SAVE_RAM || !host) return 0;
    uint32_t size;
    wc_host_get_save_data(host, &size);
    return size;
}

void* retro_get_memory_data(unsigned id) {
    if (id != RETRO_MEMORY_SAVE_RAM || !host) return NULL;
    uint32_t size;
    return wc_host_get_save_data(host, &size);
}

// ─── Misc required exports ──────────────────────────────────────────────────

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_reset(void) { /* TODO: reload cart? */ }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_set_controller_port_device(unsigned port, unsigned device) {
    (void)port; (void)device;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    (void)index; (void)enabled; (void)code;
}
