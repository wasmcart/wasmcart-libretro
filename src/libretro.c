// libretro.c — wasmcart libretro core
// Bridges RetroArch ↔ wasmcart host API (cart_host.cpp)

#ifdef _WIN32
#include <windows.h>
#endif
#include "libretro.h"
#include "../include/wasmcart_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <GLES3/gl3.h>
// Redirect this file's glXxx() (cart GL-state save/restore) to pointers loaded
// from RetroArch's get_proc_address — the core links no GL library of its own.
#include "gl_procs.h"
#include <wc_log.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0755)
#endif
#include "testcart_wasc.h"   // testcart_wasc[], testcart_wasc_len (CMakeLists.txt)

// ─── Libretro callbacks ─────────────────────────────────────────────────────

static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_log_printf_t log_cb;

// ─── Core state ─────────────────────────────────────────────────────────────

static wc_host_t* host = NULL;
// The cart file, kept for retro_reset (Restart Content loads it again).
static char game_path[4096];
// SAVE_RAM as the frontend sees it: a buffer of the core's, stable for the
// whole session. The cart's save region moves when retro_reset starts a new
// instance, and RetroArch keeps the pointer it got at load (autosave, the
// .srm written at unload). The frontend's .srm goes in before the cart's
// wc_init; the cart's region is copied back after every frame and restore.
static uint8_t* sram = NULL;
static uint32_t sram_size = 0;
static void sram_to_cart(void) {
    uint32_t n = 0;
    uint8_t* p = host ? wc_host_get_save_data(host, &n) : NULL;
    if (p && sram && n) memcpy(p, sram, n < sram_size ? n : sram_size);
}
static void sram_from_cart(void) {
    uint32_t n = 0;
    uint8_t* p = host ? wc_host_get_save_data(host, &n) : NULL;
    if (p && sram && n) memcpy(sram, p, n < sram_size ? n : sram_size);
}

// ─── Input actions → RetroArch input descriptors ─────────────────────────────
// A cart that declares input actions (wasmcart SPEC.md, "Input actions") gets
// them named in the frontend's remap screen: "Pedal" instead of "B". Origins
// are the RetroPad position on the generic pad (see lr_update_input_devices):
// RetroArch does not tell a core which physical input a RetroPad button is on.
#define WC_LR_MAX_ACTIONS 64
static wc_host_action_t lr_actions[WC_LR_MAX_ACTIONS];
static struct retro_input_descriptor lr_desc[WC_LR_MAX_ACTIONS * 4 * 4 + 1];
static int lr_action_count = -1;
static unsigned lr_frames = 0;

// wasmcart input id -> RetroPad (device, index, id); the button mapping is
// the inverse of the one the pads are read with (south = RetroPad B).
static int lr_map_input(int input, unsigned out[4][3]) {
    static const unsigned btn[14] = {
        RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3,
    };
    if (input >= 0 && input < 14) { out[0][0] = RETRO_DEVICE_JOYPAD; out[0][1] = 0; out[0][2] = btn[input]; return 1; }
    if (input == 32 || input == 33) {
        unsigned idx = input == 32 ? RETRO_DEVICE_INDEX_ANALOG_LEFT : RETRO_DEVICE_INDEX_ANALOG_RIGHT;
        out[0][0] = out[1][0] = RETRO_DEVICE_ANALOG; out[0][1] = out[1][1] = idx;
        out[0][2] = RETRO_DEVICE_ID_ANALOG_X; out[1][2] = RETRO_DEVICE_ID_ANALOG_Y;
        return 2;
    }
    if (input == 34 || input == 35) {
        out[0][0] = RETRO_DEVICE_JOYPAD; out[0][1] = 0;
        out[0][2] = input == 34 ? RETRO_DEVICE_ID_JOYPAD_L2 : RETRO_DEVICE_ID_JOYPAD_R2;
        return 1;
    }
    if (input == 36) {
        static const unsigned d[4] = { RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
                                       RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT };
        for (int i = 0; i < 4; i++) { out[i][0] = RETRO_DEVICE_JOYPAD; out[i][1] = 0; out[i][2] = d[i]; }
        return 4;
    }
    return 0;
}

// The player's device, as far as libretro tells a core: the device type the
// frontend plugged into each port (retro_set_controller_port_device; a port
// is a RetroPad until told otherwise). A RetroPad is a gamepad to the cart.
// libretro exposes no pad identity (no name, vendor or product, and no binds),
// so the family stays unknown and origins are the generic pad's labels by
// position, the same as other hosts give an unidentified pad.
#define WC_LR_PORTS 4
static unsigned lr_port_device[WC_LR_PORTS] = {
    RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD, RETRO_DEVICE_JOYPAD };
static int lr_device_sent[WC_LR_PORTS] = { -1, -1, -1, -1 };

// Which pads the cart sees connected. libretro does not say whether a port has
// a physical controller behind it (an empty port reads all zeros), so port 0
// is always connected and ports 1-3 become connected the first time they give
// any input (a button, or a stick or trigger past a quarter of its travel), as
// a browser's Gamepad API reveals a pad on its first press. A port the
// frontend sets to RETRO_DEVICE_NONE, or one at or past its number of users
// (RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS), is disconnected. Through 0.10.0 ports
// 1-3 were never connected, so a cart never saw players 2-4 at all.
static bool lr_port_seen[WC_LR_PORTS];
static unsigned lr_max_users = WC_LR_PORTS;
static bool lr_pad_connected(int p, const wc_pad_t* pad) {
    if (p == 0) return true;
    if (p >= WC_LR_PORTS || (unsigned)p >= lr_max_users || (lr_port_device[p] & RETRO_DEVICE_MASK) == RETRO_DEVICE_NONE) {
        if (p < WC_LR_PORTS) lr_port_seen[p] = false;
        return false;
    }
    const int q = 8192;
    if (pad->buttons || pad->left_x > q || pad->left_x < -q || pad->left_y > q || pad->left_y < -q ||
        pad->right_x > q || pad->right_x < -q || pad->right_y > q || pad->right_y < -q ||
        pad->left_trigger > q || pad->right_trigger > q)
        lr_port_seen[p] = true;
    return lr_port_seen[p];
}

static void lr_update_input_devices(void) {
    for (int p = 0; p < WC_LR_PORTS; p++) {
        unsigned base = lr_port_device[p] & RETRO_DEVICE_MASK;
        int dev = (base == RETRO_DEVICE_JOYPAD || base == RETRO_DEVICE_ANALOG) ? WC_HOST_DEVICE_GAMEPAD
                                                                               : WC_HOST_DEVICE_UNKNOWN;
        if (dev == lr_device_sent[p]) continue;
        lr_device_sent[p] = dev;
        wc_host_input_device(host, p, dev, 0);
    }
}

// Actions are declared in wc_init or later, so this is checked on the first
// frame and then once a second; descriptors are re-sent only on a change.
static void lr_update_input_descriptors(void) {
    if (!host || !environ_cb) return;
    if (lr_frames++ % 60) return;
    int n = wc_host_actions(host, 0, NULL, 0);
    if (n == lr_action_count || n <= 0) return;
    if (n > WC_LR_MAX_ACTIONS) n = WC_LR_MAX_ACTIONS;
    lr_action_count = n;
    wc_host_actions(host, 0, lr_actions, n);
    size_t k = 0;
    for (unsigned port = 0; port < 4; port++) {
        for (int a = 0; a < n; a++) {
            unsigned m[4][3];
            int c = lr_map_input(lr_actions[a].default_input, m);
            for (int i = 0; i < c; i++) {
                lr_desc[k].port = port; lr_desc[k].device = m[i][0];
                lr_desc[k].index = m[i][1]; lr_desc[k].id = m[i][2];
                lr_desc[k].description = lr_actions[a].name;
                k++;
            }
        }
    }
    memset(&lr_desc[k], 0, sizeof lr_desc[k]);
    if (k) environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, lr_desc);
}
static bool uses_gl = false;
static bool gl_context_ready = false;
// A 2D cart presents its framebuffer through the software video path (no
// hardware context) when the frontend has no GLES3 / GL 3.3 context for it.
static bool sw_video = false;
static struct retro_hw_render_callback hw_render;  // persists — RetroArch fills callbacks
// A GL cart on a context older than GLES 3.0: it never starts, and the frontend
// framebuffer is cleared to black instead.
static bool gl_refused = false;
static bool first_frame = true;
// A WebGPU cart's frame has been shown (present_wgpu_frame).
static bool wgpu_shown = false;
// Its frames reach GL without leaving the GPU (wc_host_wgpu_gl_share); else
// through the readback (a mapped copy, a frame late).
static bool wgpu_gl_shared = false;
// The GPU of the frontend's GL context, which Dawn is pinned to (0: unknown).
static uint32_t gl_gpu_vendor = 0, gl_gpu_device = 0;
// The player was told this cart's WebGPU device is lost.
static bool wgpu_lost_told = false;
static uint32_t cart_w = 0, cart_h = 0;
// Largest geometry we have told RetroArch about (sizes its HW render FBO).
static uint32_t max_w = 0, max_h = 0;
// on_context_reset learned the cart's real size; apply it on the next retro_run.
static bool geometry_pending = false;
// retro_reset found a cart larger than video was set up for: SET_SYSTEM_AV_INFO
// on the next retro_run (the environment call belongs in retro_run).
static bool in_reset = false, av_info_pending = false;
static uint32_t frame_count = 0;
static double time_ms = 0;


// Audio conversion buffer (F32 → S16)

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

// Read from the copy wasmcart-native's GL layer keeps as the state is set
// (gl_track.c): no glGet, each of which is a full sync with Mesa's GL thread
// (about 20 of them a frame before). The copy starts from the real state at
// context reset (wc_gl_track_reset).
static void save_cart_gl_state(void) {
    wc_gl_tracked_state t;
    wc_gl_track_get(&t);
    cart_gl_state.program = t.program;
    cart_gl_state.vao = t.vao;
    cart_gl_state.fbo = t.fbo;
    cart_gl_state.active_tex = t.active_tex;
    cart_gl_state.tex_2d = t.tex_2d;
    cart_gl_state.array_buf = t.array_buf;
    cart_gl_state.elem_buf = t.elem_buf;
    memcpy(cart_gl_state.viewport, t.viewport, sizeof cart_gl_state.viewport);
    cart_gl_state.blend = t.blend;
    cart_gl_state.depth_test = t.depth_test;
    cart_gl_state.scissor_test = t.scissor_test;
    cart_gl_state.stencil_test = t.stencil_test;
    cart_gl_state.cull_face = t.cull_face;
    cart_gl_state.dither = t.dither;
    cart_gl_state.blend_src_rgb = t.blend_src_rgb;
    cart_gl_state.blend_dst_rgb = t.blend_dst_rgb;
    cart_gl_state.blend_src_a = t.blend_src_a;
    cart_gl_state.blend_dst_a = t.blend_dst_a;
    cart_gl_state.blend_eq_rgb = t.blend_eq_rgb;
    cart_gl_state.blend_eq_a = t.blend_eq_a;
    cart_gl_state.depth_mask = t.depth_mask;
    memcpy(cart_gl_state.color_mask, t.color_mask, sizeof cart_gl_state.color_mask);
    cart_gl_state.stencil_mask = t.stencil_mask;
    memcpy(cart_gl_state.clear_color, t.clear_color, sizeof cart_gl_state.clear_color);
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
static bool jit_disabled = false;
static bool wgpu_disabled = false;
static bool perf_log_on = false;
static void perf_flush(void);

static const struct retro_core_option_v2_definition option_defs[] = {
    {
        "wasmcart_resolution",
        "Internal Resolution",
        NULL,
        "The frame size the cart is asked to render (the host's preferred size). A cart that follows it "
        "renders the largest frame of its own aspect that fits (a 16:9 game at 640x480 renders 640x360, shown at 16:9); "
        "a cart may also choose its own size. Lower is faster on a slow GPU. Applies when content is loaded or restarted (Restart Content).",
        NULL, "video",
        {
            { "480x270",   "480x270 (270p)" },
            { "640x360",   "640x360 (360p)" },
            { "640x480",   "640x480" },
            { "960x540",   "960x540 (540p)" },
            { "1280x720",  "1280x720 (720p)" },
            { "1920x1080", "1920x1080 (1080p)" },
            { "2560x1440", "2560x1440 (1440p)" },
            { "3840x2160", "3840x2160 (4K)" },
            { NULL, NULL },
        },
        "1920x1080"
    },
    {
        "wasmcart_jit",
        "Runtime Code Generation",
        NULL,
        "Lets a cart compile code while it runs (wasmcart SPEC.md, \"Runtime code generation\"). "
        "Off breaks carts built around it: they run slower or not at all, and the core says so on screen. "
        "Applies when content is loaded.",
        NULL, "system",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL },
        },
        "enabled"
    },
    {
        "wasmcart_gpu_api",
        "GPU API",
        NULL,
        "Which GPU API a cart that supports both GL and WebGPU runs on. Auto: WebGPU when this core can "
        "make a WebGPU device (Dawn on Vulkan, with the wasmcart_libretro_wgpu folder beside the core), else GL. "
        "GL: always GL; a WebGPU-only cart then does not start. Applies when content is loaded or restarted.",
        NULL, "video",
        {
            { "auto", "Auto (WebGPU when available)" },
            { "gl",   "GL" },
            { NULL, NULL },
        },
        "auto"
    },
    {
        "wasmcart_perf_log",
        "Performance Log",
        NULL,
        "Once a second, writes the frame rate and where each frame's time went (the cart's code, its GL calls, "
        "the frontend between frames, garbage collection, background threads) to RetroArch's log and to "
        "wasmcart-perf.log in the save directory; in the first 30 seconds also every frame over 50 ms. "
        "For reporting slow games. Applies when content is loaded.",
        NULL, "system",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL },
        },
        "disabled"
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
    struct retro_variable jit = { "wasmcart_jit", NULL };
    jit_disabled = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &jit) && jit.value &&
                   strcmp(jit.value, "disabled") == 0;
    struct retro_variable api = { "wasmcart_gpu_api", NULL };
    wgpu_disabled = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &api) && api.value &&
                    strcmp(api.value, "gl") == 0;
    struct retro_variable pl = { "wasmcart_perf_log", NULL };
    perf_log_on = environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pl) && pl.value && strcmp(pl.value, "enabled") == 0;
    const char* env = getenv("WASMCART_PERF_LOG");
    if (env && *env && *env != '0') perf_log_on = true;
}

// Tell the player on screen, where they are looking, and the log.
static void notify(const char* msg, enum retro_log_level level, unsigned ms) {
    if (log_cb) log_cb(level, "wasmcart: %s\n", msg);
    unsigned ver = 0;
    struct retro_message_ext ext = { msg, ms, 3, level, RETRO_MESSAGE_TARGET_ALL,
                                      RETRO_MESSAGE_TYPE_NOTIFICATION, -1 };
    if (environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &ver) && ver >= 1 &&
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &ext))
        return;
    struct retro_message legacy = { msg, ms * 60 / 1000 };
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &legacy);
}

// The off switch must be loud (SPEC.md, "Runtime code generation"): on the
// cart's first JIT call while it is off, tell the player.
static bool jit_notice_shown = false;
static void jit_notice_tick(void) {
    if (jit_notice_shown || !wc_host_jit_notice(host)) return;
    jit_notice_shown = true;
    notify("Runtime code generation is OFF (core option or WASMCART_JIT=0). This cart asked for it and runs "
           "without it: if it is slow or misbehaves, that setting is the cause.",
           RETRO_LOG_WARN, 10000);
}

// The wasmcart GL surface is OpenGL ES 3.0 (SPEC.md, "GL surface"), and the
// core's own presentation (blits, VAOs, GLSL ES 3.00) needs it too. A
// frontend asked for GLES2 may still hand back an ES3 context, so the
// version that counts is the one the context reports. Desktop GL: 3.3 core.
static bool gl_context_is_es3(char* what, size_t cap) {
    typedef const unsigned char* (*get_string_fn)(unsigned);
    get_string_fn get_string = hw_render.get_proc_address
        ? (get_string_fn)hw_render.get_proc_address("glGetString") : NULL;
    const char* v = get_string ? (const char*)get_string(GL_VERSION) : NULL;
    snprintf(what, cap, "%s", v ? v : "no GL_VERSION");
    if (!v) return false;
    int major = 0, minor = 0;
    if (strncmp(v, "OpenGL ES", 9) == 0) {
        const char* p = v + 9;
        while (*p && (*p < '0' || *p > '9')) p++;
        return sscanf(p, "%d.%d", &major, &minor) == 2 && major >= 3;
    }
    return sscanf(v, "%d.%d", &major, &minor) == 2 && (major > 3 || (major == 3 && minor >= 3));
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

    // No content (Start Core) runs the test cart (retro_load_game)
    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

// ─── Core info ──────────────────────────────────────────────────────────────

void retro_get_system_info(struct retro_system_info* info) {
    memset(info, 0, sizeof(*info));
    info->library_name = "wasmcart";
    info->library_version = "0.14.0";
    info->valid_extensions = "wasc|wasm";
    info->need_fullpath = true;   // we read the file ourselves (ZIP)
    info->block_extract = true;   // don't extract, we handle ZIP
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = cart_w;
    info->geometry.base_height = cart_h;
    // max_* sizes RetroArch's HW render FBO once, at video init. Declare the
    // larger of the frontend preference and whatever the cart said before init
    // so that on_context_reset can usually fix the real size with SET_GEOMETRY
    // (no reinit) instead of SET_SYSTEM_AV_INFO (full video reinit).
    max_w = cart_w > pref_width  ? cart_w : pref_width;
    max_h = cart_h > pref_height ? cart_h : pref_height;
    info->geometry.max_width = max_w;
    info->geometry.max_height = max_h;
    info->geometry.aspect_ratio = (float)cart_w / (float)cart_h;
    info->timing.fps = 60.0;

    const wc_cart_info_t* ci = wc_host_get_cart_info(host);
    info->timing.sample_rate = ci->audio_sample_rate ? (double)ci->audio_sample_rate : 48000.0;
}

// ─── HW render (GL carts) ───────────────────────────────────────────────────

// Tell the frontend the size the cart really has after init.
static void announce_cart_size(uint32_t w, uint32_t h) {
    // SET_SYSTEM_AV_INFO forces a full video reinit, and that tears down the
    // GL context the cart just built its shaders and textures in (context_destroy,
    // then a fresh context the cart cannot rebuild into). On Batocera/Pi5 (Mesa
    // V3D, Wayland) every GL cart then drew with GL_INVALID_OPERATION or
    // segfaulted a frame in. RetroArch's HW FBO is already sized from
    // max_w/max_h, so when the cart fits, SET_GEOMETRY -- which never reinits --
    // is all that is needed. It is applied from retro_run rather than here
    // because context_reset is still inside RetroArch's video init. Only a cart
    // that outgrows the declared max takes the reinit path; cache_context (set
    // at load) asks RetroArch to keep the context alive even then.
    if (w <= max_w && h <= max_h) {
        geometry_pending = true;
        wc_log("wasmcart: geometry %ux%u (within max %ux%u, SET_GEOMETRY on next frame)\n", w, h, max_w, max_h);
        return;
    }
    if (in_reset) {
        av_info_pending = true;
        wc_log("wasmcart: reset: %ux%u exceeds the declared max %ux%u, SET_SYSTEM_AV_INFO on next frame\n", w, h, max_w, max_h);
        return;
    }
    struct retro_system_av_info av = {0};
    av.geometry.base_width = w;
    av.geometry.base_height = h;
    av.geometry.max_width = w;
    av.geometry.max_height = h;
    av.geometry.aspect_ratio = (float)w / (float)h;
    av.timing.fps = 60.0;
    const wc_cart_info_t* ci = wc_host_get_cart_info(host);
    av.timing.sample_rate = ci->audio_sample_rate ? (double)ci->audio_sample_rate : 48000.0;
    max_w = w;
    max_h = h;
    environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    wc_log("wasmcart: SET_SYSTEM_AV_INFO %ux%u (exceeded declared max)\n", w, h);
}

static bool load_cart(void);
static void create_host(void);

// A WebGPU cart, now that the frontend's GL context exists (before wc_init):
// Dawn must be on that context's GPU, so on a machine with two the cart is
// loaded again on the right one; then its frames are shared with GL
// (zero-copy) where the platform and drivers allow, else read back. Which
// path runs is logged, and said on screen when it is the readback.
static void wgpu_with_gl(void) {
    char why[300] = "";
    uint32_t gv = 0, gd = 0, wv = 0, wd = 0;
    if (wc_gl_context_gpu(host, &gv, &gd, why, sizeof why) == 0) {
        if (wc_host_wgpu_adapter_ids(host, &wv, &wd) == 0 && (wv != gv || wd != gd) && !(gl_gpu_vendor == gv && gl_gpu_device == gd)) {
            wc_log("wasmcart: WebGPU took GPU %04x:%04x, the frontend's GL is on %04x:%04x: loading the cart again on the GL GPU\n",
                   wv, wd, gv, gd);
            gl_gpu_vendor = gv;
            gl_gpu_device = gd;
            wc_host_exit_v8();
            wc_host_destroy(host);
            host = NULL;
            create_host();
            if (!host || !load_cart()) {
                notify("The cart could not be loaded again on the frontend's GPU.", RETRO_LOG_ERROR, 10000);
                if (host) { wc_host_destroy(host); host = NULL; }
                return;
            }
            wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)hw_render.get_proc_address);
        }
        gl_gpu_vendor = gv;
        gl_gpu_device = gd;
    } else {
        wc_log("wasmcart: WebGPU: which GPU the frontend's GL is on is unknown (%s); WebGPU stays on %s\n",
               why, wc_host_wgpu_adapter(host));
    }
    const char* off = getenv("WASMCART_NO_WGPU_SHARE");  // tests: the readback path
    why[0] = 0;
    if (off && *off && *off != '0') snprintf(why, sizeof why, "WASMCART_NO_WGPU_SHARE is set");
    wgpu_gl_shared = !why[0] && wc_host_wgpu_gl_share(host, why, sizeof why) == 0;
    if (wgpu_gl_shared) {
        wc_log("wasmcart: WebGPU frames reach GL on the GPU, zero-copy (shared memory), the same frame\n");
    } else {
        wc_log("wasmcart: WebGPU frames reach GL by READBACK (GPU -> CPU -> GPU, one frame late): %s\n", why);
        char msg[400];
        snprintf(msg, sizeof msg, "WebGPU frames are copied through the CPU (slower): %s", why);
        notify(msg, RETRO_LOG_WARN, 6000);
    }
}

// GL is available: finish the cart's deferred init (wc_init), size the
// redirect target to what it settled on and tell the frontend.
static void finish_cart_init(void) {
    if (!host) return;

    // RetroArch is the host: give wasmcart its GL via the frontend's
    // get_proc_address (the common libretro GL context — GLES3 on Android /
    // handhelds, whatever the desktop frontend provides). The shared GL layer
    // loads its entry points from this, so the core links NO GL library of
    // its own (no ANGLE, single self-contained file).
    wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)hw_render.get_proc_address);
    // A WebGPU cart gets no GL imports, so the GL layer never loads its entry
    // points from the cart's side; the core's present (the redirect FBO and
    // the frame upload) needs them.
    if (wc_host_uses_wgpu(host)) wc_gl_procs_load((void*)hw_render.get_proc_address);
    if (wc_host_uses_wgpu(host)) wgpu_with_gl();
    if (!host) return;

    // Create redirect FBO with depth+stencil BEFORE cart init.
    // Three.js needs depth testing, Ganesh needs stencil.
    // RetroArch's hw_render FBO may not have these attachments.
    extern void wc_gl_setup_redirect(uint32_t w, uint32_t h);
    wc_gl_setup_redirect(pref_width, pref_height);
    // the GL state save / restore keeps a copy as it is set: start it from
    // what the frontend's context really has
    wc_gl_track_reset();

    sram_to_cart();  // the frontend's .srm (copied in after load) before wc_init
    wc_host_finish_init(host);
    sram_from_cart();

    // Re-read dimensions (may have changed after init)
    const wc_cart_info_t* ci = wc_host_get_cart_info(host);
    cart_w = ci->width;
    cart_h = ci->height;

    // Sizing differs by cart TYPE, and taking max() for both was wrong.
    //
    // A GL cart renders into whatever drawable it is handed, so it should
    // get the larger of its own request and the frontend preference --
    // undersizing crops it.
    //
    // A 2D cart is the opposite: it draws into a fixed cart_w x cart_h
    // framebuffer that we upload as a texture. Sizing the target to the
    // preference does NOT scale it up; it leaves the cart's pixels in the
    // bottom-left corner of a mostly-empty buffer. The 64x64 rumble
    // fixture landed in 0.1% of a 1920x1080 target -- a black screen with a
    // speck in the corner. Give a 2D cart exactly its own size and let
    // RetroArch scale it to the window, which is what frontends are for.
    // The target must match what the cart ACTUALLY renders, for both cart
    // types. Taking max() with the frontend preference was wrong: it does
    // not scale the cart up, it leaves the cart's output in the bottom-left
    // corner of a larger, mostly-empty frame.
    //
    // Measured: hello_canvas declares 800x600 and its Ganesh blit really is
    // 800x600, but the FBO was sized 1920x1080 -- so 75% of the screen was
    // empty and the image sat in the corner. The 64x64 rumble fixture was
    // the same bug at an extreme, showing as an all-black screen.
    //
    // pref_width/height still reach the cart through wc_host_options_t as a
    // PREFERENCE before init; a cart free to honour it comes back already
    // resized, and cart_w/h then equals the preference anyway. What a cart
    // reports after init is the authority, so use it verbatim and let
    // RetroArch scale to the window -- that is what a frontend is for.
    wc_gl_setup_redirect(cart_w, cart_h);
    announce_cart_size(cart_w, cart_h);

    // Save cart's initial GL state so restore works on first frame
    save_cart_gl_state();
}

static void on_context_reset(void) {
    if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: GL context reset\n");

    char version[160];
    if (!gl_context_is_es3(version, sizeof version)) {
        if (!uses_gl) {
            // A frontend that said yes to GLES3 / GL core and then gave less:
            // a 2D cart does not need GL, so it presents in software.
            sw_video = true;
            wc_log("wasmcart: context is \"%s\", not GLES 3.0: 2D cart presents in software\n", version);
            return;
        }
        gl_refused = true;
        char msg[320];
        snprintf(msg, sizeof msg,
                 "This cart needs OpenGL ES 3.0 (GLES 3.0 required); the frontend's GL context is \"%s\". "
                 "The cart is not started.", version);
        notify(msg, RETRO_LOG_ERROR, 30000);
        return;
    }
    gl_context_ready = true;

    // Get RetroArch's hw render callbacks
    struct retro_hw_render_callback* hw_cb = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &hw_cb)) {
        // Not needed for basic operation
    }

    // Now GL is available — finish deferred init
    finish_cart_init();
}

static void on_context_destroy(void) {
    if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: GL context destroyed\n");
    // the shared frame's GL half goes with the context (the next reset shares again)
    if (host && wgpu_gl_shared) wc_host_wgpu_gl_unshare(host);
    wgpu_gl_shared = false;
}

// ─── Init / Deinit ──────────────────────────────────────────────────────────

// A host with the frontend's rumble behind it (retro_init; retro_reset makes a fresh one).
static void create_host(void) {
    host = wc_host_create();
    if (!host && log_cb)
        log_cb(RETRO_LOG_ERROR, "wasmcart: failed to create host\n");
    if (host && rumble_available) {
        wc_rumble_backend_t backend = {
            .has_rumble = lr_has_rumble,
            .rumble     = lr_rumble,
            .stop       = lr_rumble_stop,
            .user       = NULL,
        };
        wc_host_set_rumble_backend(host, &backend);
    }
}

void retro_init(void) {

    // Rumble is optional: a frontend without it simply says no, and the cart's
    // wc_pad_has_rumble then reports 0 rather than the cart failing to load.
    rumble_available = environ_cb &&
        environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble_iface);
    memset(rumble_frames_left, 0, sizeof(rumble_frames_left));
    create_host();
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "wasmcart: rumble %s\n",
               rumble_available ? "available" : "not supported by frontend");
}

void retro_deinit(void) {
    perf_flush();
    for (uint32_t i = 0; i < WC_MAX_PADS; i++) lr_rumble_stop(NULL, i);
    wc_host_exit_v8();
    wc_host_destroy(host);
    host = NULL;
    free(sram);
    sram = NULL;
    sram_size = 0;
}

// ─── Load game ──────────────────────────────────────────────────────────────

// Load game_path into the host with the current core options; wc_init waits
// for the GL context (context_reset, or the first frame). SAVE_RAM is the
// core's buffer: the cart gets its bytes before wc_init.
static bool load_cart(void) {
    // For GL carts, defer _initialize/wc_init until GL context is ready
    wc_host_options_t opts = {0};
    opts.host_fps = 60;
    opts.audio_sample_rate = 48000;
    check_options();
    opts.preferred_width = pref_width;
    opts.preferred_height = pref_height;
    opts.jit_disabled = jit_disabled;
    opts.wgpu_disabled = wgpu_disabled;
    jit_notice_shown = false;
    wgpu_shown = false;
    wgpu_lost_told = false;
    opts.defer_init = true;  // always defer — we'll finish in context_reset or first frame
    opts.save_data = sram;
    opts.save_data_size = sram ? sram_size : 0;
    // A WebGPU cart: its frames shared with the frontend's GL where they can
    // be, on the GL context's GPU once that is known (finish_cart_init).
    opts.wgpu_gl_share = true;
    opts.wgpu_adapter_vendor = gl_gpu_vendor;
    opts.wgpu_adapter_device = gl_gpu_device;
    // the cart's compiled code, kept between runs in the frontend's save directory
    static char code_cache[1024];
    const char* save_dir = NULL;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir && *save_dir) {
        snprintf(code_cache, sizeof code_cache, "%s/wasmcart/code", save_dir);
        opts.code_cache_dir = code_cache;
    }

    int rc = wc_host_load_file(host, game_path, &opts);
    if (rc != 0) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "wasmcart: failed to load %s\n", game_path);
        const char* why = wc_host_wgpu_unavailable_reason(host);
        if (why && *why) {
            // A WebGPU-only cart on a host without WebGPU: say why, where the player looks.
            char msg[400];
            snprintf(msg, sizeof msg, "This cart needs WebGPU, which is not available: %s", why);
            notify(msg, RETRO_LOG_ERROR, 15000);
        }
        return false;
    }

    // Hold V8 locker persistently — RetroArch calls retro_run from the same thread
    wc_host_enter_v8();

    const wc_cart_info_t* info = wc_host_get_cart_info(host);
    cart_w = info->width;
    cart_h = info->height;
    uses_gl = wc_host_uses_gl(host);

    // The save region is known at load (wc_get_info ran): the frontend's
    // buffer has its size, and the cart's bytes until the frontend's .srm lands.
    uint32_t n = 0;
    uint8_t* region = wc_host_get_save_data(host, &n);
    if (!sram && n) {
        sram = (uint8_t*)calloc(1, n);
        sram_size = sram ? n : 0;
        if (sram && region) memcpy(sram, region, n);
    }
    return true;
}

// No content (RetroArch's Start Core): the test cart, wasmcart's testcart/
// compiled in (testcart/README.md): pads, pointer, keyboard, sound and saves
// on one screen. It is written to <save directory>/wasmcart/testcart.wasc
// (the system directory when there is no save directory) and loaded from
// there like any cart, so Restart Content, the code cache and SAVE_RAM work
// the same. The file is rewritten only when it differs from the cart this
// core carries.
static bool testcart_file(char* out, size_t cap) {
    const char* dir = NULL;
    if (!(environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir) &&
        !(environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir && *dir)) {
        notify("No content: the test cart needs a save or system directory to run from.", RETRO_LOG_ERROR, 10000);
        return false;
    }
    char sub[3800];
    if ((size_t)snprintf(sub, sizeof sub, "%s/wasmcart", dir) >= sizeof sub) {
        notify("No content: the save directory path is too long for the test cart.", RETRO_LOG_ERROR, 10000);
        return false;
    }
    make_dir(sub);
    snprintf(out, cap, "%s/testcart.wasc", sub);
    FILE* f = fopen(out, "rb");
    if (f) {
        size_t i = 0;
        int ch;
        bool same = true;
        while ((ch = fgetc(f)) != EOF)
            if (i >= testcart_wasc_len || (unsigned char)ch != testcart_wasc[i++]) { same = false; break; }
        fclose(f);
        if (same && i == testcart_wasc_len) return true;
    }
    f = fopen(out, "wb");
    bool ok = f && fwrite(testcart_wasc, 1, testcart_wasc_len, f) == testcart_wasc_len;
    if (f && fclose(f) != 0) ok = false;
    if (!ok) {
        char msg[4200];
        snprintf(msg, sizeof msg, "No content: the test cart could not be written to %s", out);
        notify(msg, RETRO_LOG_ERROR, 10000);
    }
    return ok;
}

bool retro_load_game(const struct retro_game_info* game) {
    lr_action_count = -1;   // input descriptors are re-sent for the new cart
    for (int p = 0; p < WC_LR_PORTS; p++) lr_device_sent[p] = -1;  // and devices
    lr_frames = 0;
    memset(lr_port_seen, 0, sizeof lr_port_seen);
    if (!host) return false;
    if (game && game->path && *game->path) {
        snprintf(game_path, sizeof game_path, "%s", game->path);
    } else {
        if (!testcart_file(game_path, sizeof game_path)) return false;
        wc_log("wasmcart: no content: the test cart, %s\n", game_path);
    }

    // Set up file logging next to the cart (Android: logcat buffer too small)
    {
        const char* slash = strrchr(game_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - game_path);
            char log_path[1024];
            snprintf(log_path, sizeof(log_path), "%.*s/wasmcart.log", (int)dir_len, game_path);
            wc_log_set_file(log_path);
        }
    }

    // Set pixel format for 2D carts
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

    if (!load_cart()) return false;

    // Which GPU API a cart that imports WebGPU got, on screen once at load.
    {
        char msg[400];
        const char* why = wc_host_wgpu_unavailable_reason(host);
        if (wc_host_uses_wgpu(host)) {
            snprintf(msg, sizeof msg, "WebGPU on %s", wc_host_wgpu_adapter(host));
            notify(msg, RETRO_LOG_INFO, 4000);
        } else if (why && *why) {
            snprintf(msg, sizeof msg, "GL (no WebGPU: %s)", why);
            notify(msg, RETRO_LOG_INFO, 4000);
        }
    }

    const wc_manifest_t* manifest = wc_host_get_manifest(host);
    if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: loaded %s (%ux%u, %s)\n",
        manifest->name, cart_w, cart_h, wc_host_uses_wgpu(host) ? "WebGPU" : uses_gl ? "GL" : "2D");

    // Request HW render — unified GL display path for all carts. 2D carts get
    // their framebuffer uploaded to the redirect FBO as a texture, which needs
    // GLES3 or GL 3.3 core like a GL cart does; without one they present
    // through the software video path instead.
    sw_video = false;
    gl_refused = false;
    gl_context_ready = false;
    first_frame = true;
    memset(&hw_render, 0, sizeof(hw_render));
    hw_render.context_reset = on_context_reset;
    hw_render.context_destroy = on_context_destroy;
    hw_render.bottom_left_origin = true;
    hw_render.depth = true;
    hw_render.stencil = true;
    // Keep the GL context across video reinits (fullscreen toggle, av_info
    // change): the cart's GL objects live in it and cannot be recreated.
    hw_render.cache_context = true;

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
        } else if (uses_gl) {
            // GLES2-only frontends (old handhelds such as a PowerVR SGX544).
            // Some drivers hand back an ES3 context for an ES2 request:
            // on_context_reset checks the version the context reports, and
            // refuses the cart if it is below ES 3.0.
            hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
            hw_render.version_major = 2;
            hw_render.version_minor = 0;
            got_context = environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render);
        }
    }
    if (!got_context && !uses_gl) {
        memset(&hw_render, 0, sizeof(hw_render));
        sw_video = true;
        if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart: no GLES3 / GL 3.3 context: 2D cart presents in software\n");
    } else if (!got_context) {
        notify("This cart needs OpenGL ES 3.0 (GLES 3.0 required), and the frontend offers no GL context "
               "for it. The cart is not started.", RETRO_LOG_ERROR, 30000);
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
    perf_flush();
}

// ─── WebGPU present ─────────────────────────────────────────────────────────

// The cart rendered on Dawn's own device. Its frame reaches RetroArch's GL
// framebuffer through a mapped buffer, pipelined: the copy of frame N is
// queued as soon as it is rendered, and frame N-1 (copied while N rendered)
// is drawn into the redirect FBO. One frame of latency; the core only waits
// when the GPU has not finished N-1 by the next retro_run. The frame may be
// a different size than wc_get_info said (the cart sizes its canvas itself);
// the redirect target and the frontend follow it.
// Per frame, for the performance log: the cart's wc_render (with the glue),
// queueing the copy, waiting for the frame to show, the GL upload, and the
// latency of the frame shown (queue to mapped: GPU render + copy).
static struct { double cart, queue, wait, upload, latency; bool shown; } wgpu_t;
static double perf_now_ms(void);
static void present_wgpu_frame(void) {
    // A lost device (a driver reset, the GPU gone): the cart cannot go on;
    // the last frame stays on screen and the player is told why, once.
    const char* lost = wc_host_gpu_lost(host);
    if (lost) {
        if (!wgpu_lost_told) {
            wgpu_lost_told = true;
            char msg[400];
            snprintf(msg, sizeof msg, "The GPU device was lost (%s). The cart is stopped; Restart Content starts it again.", lost);
            notify(msg, RETRO_LOG_ERROR, 15000);
        }
        return;
    }
    double t0 = perf_now_ms();
    if (wgpu_gl_shared) {
        // This frame, copied on the GPU into the texture GL shares; GL waits for the copy.
        wc_wgpu_gl_frame_t g;
        char why[300] = "";
        int r = wc_host_wgpu_gl_frame(host, &g, why, sizeof why);
        double t1s = perf_now_ms();
        wgpu_t.queue = t1s - t0;
        wgpu_t.wait = 0;
        wgpu_t.latency = 0;
        wgpu_t.shown = r == 0;
        if (r == 0) {
            if (g.width != cart_w || g.height != cart_h) {
                extern void wc_gl_setup_redirect(uint32_t w, uint32_t h);
                cart_w = g.width;
                cart_h = g.height;
                wc_gl_setup_redirect(g.width, g.height);
                announce_cart_size(g.width, g.height);
            }
            extern void wc_gl_present_texture(uint32_t tex, uint32_t w, uint32_t h, int bgra);
            wc_gl_present_texture(g.texture, g.width, g.height, g.bgra);
            wc_host_wgpu_gl_frame_done(host);
            wgpu_shown = true;
            wgpu_t.upload = perf_now_ms() - t1s;
            return;
        }
        if (r == 1) return;  // no frame yet
        wgpu_gl_shared = false;
        wc_log("wasmcart: WebGPU frame sharing failed (%s); frames reach GL by READBACK from now on\n", why);
    }
    wc_host_wgpu_frame_queue(host);
    double t1 = perf_now_ms();
    wc_wgpu_frame_t f;
    // Nothing shown yet: wait for this very frame rather than show black.
    int took = wc_host_wgpu_frame_take(host, &f, wgpu_shown ? 1 : 0, 250);
    double t2 = perf_now_ms();
    wgpu_t.queue = t1 - t0;
    wgpu_t.wait = t2 - t1;
    wgpu_t.shown = took == 0;
    if (took != 0) return;
    wgpu_t.latency = f.latency_ms;
    if (f.width != cart_w || f.height != cart_h) {
        extern void wc_gl_setup_redirect(uint32_t w, uint32_t h);
        cart_w = f.width;
        cart_h = f.height;
        wc_gl_setup_redirect(f.width, f.height);
        announce_cart_size(f.width, f.height);
    }
    extern void wc_gl_upload_frame(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t row_pixels, int bgra);
    wc_gl_upload_frame(f.data, f.width, f.height, f.bytes_per_row / 4, f.bgra);
    wc_host_wgpu_frame_release(host);
    wgpu_shown = true;
    wgpu_t.upload = perf_now_ms() - t2;
}

// ─── Run frame ──────────────────────────────────────────────────────────────

// Time spent in the frontend's callbacks this frame, for the performance log:
// RetroArch can block in them (audio sync waiting for its queue to drain,
// video_cb on a swap), which the cart's own times do not show.
static struct { double video, audio, input; unsigned samples; } fe_t;
// Where retro_run's time went this frame (the performance log's phases):
// input (polling RetroArch and building the pads), the cart's GL state put
// back, wc_host_run_frame, presenting (the WebGPU frame into GL, the blit,
// the GL state saved and reset for RetroArch, video_cb) and audio.
static struct { double input, glstate, frame, present, audio; } ph_t;
#define FE_CALL(slot, call) do { double fe_t0_ = perf_now_ms(); call; fe_t.slot += perf_now_ms() - fe_t0_; } while (0)
static void run_frame(double* delta_out);
static void perf_frame(double t_enter, double t_exit, double delta_ms);
#ifdef _WIN32
static double perf_now_ms(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1e3 / (double)freq.QuadPart;
}
#else
static double perf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
}
#endif

void retro_run(void) {
    if (!perf_log_on) { run_frame(NULL); return; }
    double delta = 0, t0 = perf_now_ms();
    memset(&fe_t, 0, sizeof fe_t);
    memset(&ph_t, 0, sizeof ph_t);
    run_frame(&delta);
    perf_frame(t0, perf_now_ms(), delta);
}

static void run_frame(double* delta_out) {
    if (!host) return;
    if (gl_refused) {
        // The cart never starts. Clear the frontend's framebuffer (ES 2.0
        // calls only) so the screen is black, not whatever it held.
        static bool procs = false;
        if (!procs) { wc_gl_procs_load((void*)hw_render.get_proc_address); procs = true; }
        if (hw_render.get_current_framebuffer && p_glClear) {
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)hw_render.get_current_framebuffer());
            glViewport(0, 0, cart_w, cart_h);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            FE_CALL(video, video_cb(RETRO_HW_FRAME_BUFFER_VALID, cart_w, cart_h, 0));
        }
        return;
    }
    jit_notice_tick();  // also a JIT call in wc_init, or one that then trapped
    if (wc_host_has_trapped(host)) return;

    // Safety fallback: finish init if context_reset wasn't called
    if (first_frame) {
        first_frame = false;
        if (!gl_context_ready) {
            sram_to_cart();
            wc_host_finish_init(host);
            sram_from_cart();
            const wc_cart_info_t* ci = wc_host_get_cart_info(host);
            cart_w = ci->width;
            cart_h = ci->height;
            if (sw_video) announce_cart_size(cart_w, cart_h);
        }
    }

    // Run rumble durations down before polling, so an effect started last
    // frame stops on time even if the cart does not call rumble again.
    rumble_tick();
    double ph0 = perf_log_on ? perf_now_ms() : 0;

    // 1. Poll input
    FE_CALL(input, input_poll_cb());

    // 2. Translate RetroArch input → wasmcart pads
    {
        unsigned users = 0; // the frontend's player count can change while running
        if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS, &users)) lr_max_users = users;
    }
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
        // WC_BUTTON_GUIDE, MISC1, PADDLE1-4 and TOUCHPAD (ABI v4) are NOT set
        // here, deliberately: the RetroPad abstraction has no ids for them.
        // Its button set stops at a SNES-style pad plus L2/R2/L3/R3, so there
        // is nothing to read -- polling for them would be dead code that
        // always returns 0. A cart wanting paddles gets them from a host that
        // can actually see them, and reads 0 here, which is the same as a pad
        // that does not have any.

        pads[p].buttons = buttons;
        pads[p].left_x = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
        pads[p].left_y = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
        pads[p].right_x = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
        pads[p].right_y = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
        // Triggers pass straight through as of ABI v4: libretro reports
        // analog buttons as 0..0x7fff (libretro.h: "Analog buttons are
        // reported in the range of [0, 0x7fff]") and the pad struct now
        // stores int16 0..32767, which is the same range.
        //
        // This used to narrow into a uint8 with `>> 8`, which divides by 256
        // when the range only spans 15 bits: a fully pressed trigger read as
        // 127 of 255, so it never looked more than half pressed and a cart
        // gating on a high threshold never fired. There is no conversion left
        // to get wrong.
        pads[p].left_trigger  = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
        pads[p].right_trigger = input_state_cb(p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2);
        pads[p].connected = lr_pad_connected(p, &pads[p]);
        if (!pads[p].connected) memset(&pads[p], 0, sizeof pads[p]);
    }
    wc_host_set_pads(host, pads);

    /* ── Pointer and mouse ────────────────────────────────────────────────
     *
     * TWO libretro DEVICES, because neither covers the wasmcart pointer ABI
     * on its own:
     *
     *   RETRO_DEVICE_POINTER carries absolute position and works for touch.
     *     Its coordinates are [-0x7fff, 0x7fff] across the GAME IMAGE, so
     *     RetroArch has already undone its own letterboxing and scaling for
     *     us: no window arithmetic here, which is the part every other host
     *     has to get right by hand. _PRESSED is 1 only INSIDE the image,
     *     which is exactly the ABI's `active`.
     *
     *   RETRO_DEVICE_MOUSE carries the buttons POINTER has no room for
     *     (middle, right) and the wheel. Its X/Y are RELATIVE deltas, so it
     *     is deliberately not used for position.
     *
     * A frontend with neither configured returns 0 for all of it, which
     * leaves the pointer inactive -- the same shape as a device with no
     * mouse, and what a cart already has to handle.
     */
    {
        const int16_t px = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
        const int16_t py = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
        const bool    pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;

        const bool m_left   = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT)   != 0;
        const bool m_right  = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT)  != 0;
        const bool m_middle = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE) != 0;

        // Bit 0 primary, bit 1 secondary, bit 2 middle (SPEC.md). POINTER's
        // single _PRESSED folds into the primary bit: on a desktop the
        // frontend maps it to the left button, and on a touchscreen a press
        // IS the primary button.
        uint8_t buttons = 0;
        if (pressed || m_left) buttons |= 0x01;
        if (m_right)           buttons |= 0x02;
        if (m_middle)          buttons |= 0x04;

        // POINTER is [-0x7fff, 0x7fff] across the game image; the cart wants
        // pixels in its own resolution. Clamped to the last row and column so
        // the far edge cannot land one pixel outside the framebuffer.
        uint32_t cw = 0, ch = 0;
        (void)wc_host_get_framebuffer(host, &cw, &ch);
        // GL and WebGPU carts have no framebuffer: their frame is cart_w x cart_h.
        if (cw == 0 || ch == 0) { cw = cart_w; ch = cart_h; }
        if (cw == 0) cw = 1;
        if (ch == 0) ch = 1;
        long cx = ((long)px + 0x7fff) * (long)cw / 0xfffe;
        long cy = ((long)py + 0x7fff) * (long)ch / 0xfffe;
        if (cx < 0) cx = 0; else if (cx >= (long)cw) cx = (long)cw - 1;
        if (cy < 0) cy = 0; else if (cy >= (long)ch) cy = (long)ch - 1;

        // Active when the frontend says the pointer is on the image, or when
        // any mouse button is down. A pointer that is merely hovering with no
        // press still reports a position, which is what a desktop cart wants.
        const uint8_t active = (pressed || buttons || px != 0 || py != 0) ? 1 : 0;
        wc_host_set_pointer(host, 0, (int16_t)cx, (int16_t)cy, buttons, active);

        // Wheel. libretro exposes it as four one-shot directional flags
        // rather than a delta, so each set flag is one notch. The host
        // accumulates and zeroes per frame.
        int32_t wdx = 0, wdy = 0;
        if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP))         wdy += WC_WHEEL_DELTA;
        if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN))       wdy -= WC_WHEEL_DELTA;
        if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP))   wdx += WC_WHEEL_DELTA;
        if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN)) wdx -= WC_WHEEL_DELTA;
        if (wdx || wdy) wc_host_add_wheel(host, wdx, wdy);
    }

    double ph1 = perf_log_on ? perf_now_ms() : 0;
    ph_t.input = ph1 - ph0;

    // 3. Set time: one retro_run is one frame of the frontend's timing (60 fps),
    // so a cart advances 1000/60 ms and makes that much audio. The frontend's
    // measured frame time is NOT used: a cart makes audio for the dt it is given
    // (cartwheel: audio_ms_owed += delta_ms), so one slow frame made up to 66 ms
    // of audio, RetroArch's audio sync blocked ~66 ms to play it, the next frame
    // measured 66 ms again, and the game held at 15 fps until dynamic rate
    // control drained it (15 -> 23 -> 47 -> 60 fps over ~37 s on a Vega 8 box).
    // RetroArch paces retro_run and handles fast-forward / slow-motion itself.
    const double FIXED_STEP = 1000.0 / 60.0;
    double delta_ms = FIXED_STEP;
    time_ms += delta_ms;
    if (delta_out) *delta_out = delta_ms;
    wc_host_set_time(host, time_ms, delta_ms, frame_count);
    frame_count++;

    if (av_info_pending) {
        av_info_pending = false;
        announce_cart_size(cart_w, cart_h);
    }
    // Deferred from on_context_reset: geometry change without a video reinit.
    if (geometry_pending) {
        geometry_pending = false;
        struct retro_game_geometry geom = {0};
        geom.base_width = cart_w;
        geom.base_height = cart_h;
        geom.max_width = max_w;
        geom.max_height = max_h;
        geom.aspect_ratio = (float)cart_w / (float)cart_h;
        environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
        wc_log("wasmcart: SET_GEOMETRY %ux%u\n", cart_w, cart_h);
    }

    // DIRECT PRESENT: a GL cart draws straight into RetroArch's hw_render
    // framebuffer instead of the redirect FBO, so the per-frame copy into it
    // is skipped (RetroArch's own present pass remains). Only when that
    // framebuffer has the depth and stencil a 3D cart needs (we ask for both,
    // a driver may still not give them); WASMCART_NO_DIRECT=1 keeps the old
    // path. The target is refreshed every frame: the frontend may change it.
    static int direct_state = -1; /* -1 undecided, 0 redirect, 1 direct */
    if (gl_context_ready && uses_gl && hw_render.get_current_framebuffer) {
        extern void wc_gl_set_direct_target(uint32_t fbo);
        extern void wc_gl_set_direct(int on);
        uint32_t ra = (uint32_t)hw_render.get_current_framebuffer();
        if (direct_state < 0) {
            /* attachment sizes (GL_DEPTH_BITS is not valid in a core profile) */
            GLint depth = 0, stencil = 0, type = GL_NONE;
            glBindFramebuffer(GL_FRAMEBUFFER, ra);
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (type != GL_NONE) glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth);
            type = GL_NONE;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
            if (type != GL_NONE) glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &stencil);
            while (glGetError() != GL_NO_ERROR) { }
            const char* off = getenv("WASMCART_NO_DIRECT");
            direct_state = depth > 0 && stencil > 0 && !(off && *off && *off != '0');
            wc_log("wasmcart: %s (frontend FBO depth %d, stencil %d)\n",
                   direct_state ? "direct present into RetroArch's FBO" : "presenting through the redirect FBO",
                   depth, stencil);
        }
        if (direct_state == 1) {
            wc_gl_set_direct_target(ra);
            wc_gl_set_direct(1);
        }
    }

    // 4. Run one frame — restore cart's GL state before rendering
    double ph2 = perf_log_on ? perf_now_ms() : 0;
    if (gl_context_ready) {
        restore_cart_gl_state();
        extern void wc_gl_rebind_redirect(void);
        wc_gl_rebind_redirect();
    }
    lr_update_input_devices();
    double t_cart = perf_now_ms();
    ph_t.glstate = t_cart - ph2;
    wc_host_run_frame(host);
    wgpu_t.cart = perf_now_ms() - t_cart;
    ph_t.frame = wgpu_t.cart;
    double ph3 = perf_log_on ? perf_now_ms() : 0;
    sram_from_cart();
    jit_notice_tick();
    lr_update_input_descriptors();

    if (wc_host_has_trapped(host)) {
        if (log_cb) log_cb(RETRO_LOG_ERROR, "wasmcart: cart trapped\n");
        // said on screen once: why the cart stopped (a cart thread that died, ...)
        static wc_host_t* told = NULL;
        const char* why = wc_host_trap_reason(host);
        if (why && told != host) {
            told = host;
            char msg[600];
            snprintf(msg, sizeof msg, "The cart stopped: %s", why);
            notify(msg, RETRO_LOG_ERROR, 15000);
        }
        return;
    }

    // 5. Present video — unified GL path for all carts
    if (sw_video) {
        // The cart's ARGB8888 framebuffer is XRGB8888 to libretro as it is.
        uint32_t fb_w, fb_h;
        const uint8_t* fb = wc_host_get_framebuffer(host, &fb_w, &fb_h);
        if (fb && fb_w > 0 && fb_h > 0) FE_CALL(video, video_cb(fb, fb_w, fb_h, (size_t)fb_w * 4));
        else FE_CALL(video, video_cb(NULL, cart_w, cart_h, 0));
    } else if (hw_render.get_current_framebuffer) {
        uintptr_t ra_fbo = hw_render.get_current_framebuffer();

        // WebGPU carts: the cart rendered on Dawn's own device; its frame
        // comes back through a mapped buffer and is drawn into the redirect
        // FBO like a 2D cart's framebuffer.
        if (!uses_gl && wc_host_uses_wgpu(host)) {
            present_wgpu_frame();
        } else if (!uses_gl) {
            uint32_t fb_w, fb_h;
            const uint8_t* fb = wc_host_get_framebuffer(host, &fb_w, &fb_h);
            if (fb && fb_w > 0 && fb_h > 0) {
                extern void wc_gl_upload_framebuffer(const uint8_t* pixels, uint32_t w, uint32_t h);
                wc_gl_upload_framebuffer(fb, fb_w, fb_h);
            }
        }

        /* WASMCART_SHOT=<frame>:<file.ppm>: save that frame as the cart drew
         * it (direct target or redirect), for tests */
        {
            static long shot_n = -2, frames_run = 0;
            static char shot_path[512];
            if (shot_n == -2) {
                const char* e = getenv("WASMCART_SHOT");
                const char* colon = e ? strchr(e, ':') : NULL;
                shot_n = colon ? atol(e) : -1;
                if (colon) snprintf(shot_path, sizeof shot_path, "%s", colon + 1);
            }
            if ((uses_gl || wc_host_uses_wgpu(host)) && frames_run == shot_n) {
                extern int wc_gl_read_frame(uint8_t* out, uint32_t w, uint32_t h);
                uint8_t* px = (uint8_t*)malloc((size_t)cart_w * cart_h * 4);
                FILE* f = px && wc_gl_read_frame(px, cart_w, cart_h) ? fopen(shot_path, "wb") : NULL;
                if (f) {
                    fprintf(f, "P6\n%u %u\n255\n", cart_w, cart_h);
                    for (uint32_t y = 0; y < cart_h; y++)
                        for (uint32_t x = 0; x < cart_w; x++) fwrite(px + ((size_t)(cart_h - 1 - y) * cart_w + x) * 4, 1, 3, f);
                    fclose(f);
                    wc_log("wasmcart: frame %ld -> %s\n", shot_n, shot_path);
                }
                free(px);
            }
            frames_run++;
        }

        extern int wc_gl_has_redirect(void);
        extern void wc_gl_get_blit_size(uint32_t* w, uint32_t* h);
        uint32_t blit_w = 0, blit_h = 0;
        wc_gl_get_blit_size(&blit_w, &blit_h);
        // Same rule as the redirect sizing above: fall back to the cart's own
        // resolution, never max() with the preference.
        if (!blit_w) blit_w = cart_w;
        if (!blit_h) blit_h = cart_h;

        if (wc_gl_has_redirect()) {
            extern void wc_gl_blit_to_fbo(uint32_t target_fbo, uint32_t cart_w, uint32_t cart_h, uint32_t dst_w, uint32_t dst_h, int flip_y);
            wc_gl_blit_to_fbo((uint32_t)ra_fbo, blit_w, blit_h, blit_w, blit_h, 0);
        }

        // Save cart's GL state, then reset for RetroArch overlay/menu
        save_cart_gl_state();
        reset_gl_for_retroarch();

        FE_CALL(video, video_cb(RETRO_HW_FRAME_BUFFER_VALID, blit_w, blit_h, 0));
    }

    // 6. Send audio
    double ph4 = perf_log_on ? perf_now_ms() : 0;
    ph_t.present = ph4 - ph3;
    // int16 straight from the cart's ring (libretro takes only int16), one pass
    uint32_t num_frames;
    const int16_t* audio = wc_host_get_audio_s16(host, &num_frames);
    fe_t.samples += num_frames;
    if (num_frames > 0 && audio_batch_cb) FE_CALL(audio, audio_batch_cb(audio, num_frames));
    if (perf_log_on) ph_t.audio = perf_now_ms() - ph4;
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
    sram_from_cart();
    // JIT slots are never carried across a restore: unlink them all and change
    // the restore generation (wc_jit_config key 7) before the cart runs again.
    wc_host_jit_unlink_all(host);
    return true;
}

// ─── Persistent save (SRAM) ─────────────────────────────────────────────────

size_t retro_get_memory_size(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM ? sram_size : 0;
}

void* retro_get_memory_data(unsigned id) {
    return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL;
}

// ─── Misc required exports ──────────────────────────────────────────────────

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
// Restart Content: a fresh instance of the cart, as a console's reset button
// restarts its game: the old instance (and the GL objects it left in the
// frontend's context) goes, the cart is loaded again with the current core
// options (Internal Resolution among them) and starts from wc_init with the
// current SAVE_RAM. Its size goes to the frontend: SET_GEOMETRY when it fits
// what video was set up with, else SET_SYSTEM_AV_INFO.
void retro_reset(void) {
    if (!host || !game_path[0] || gl_refused) return;
    bool init_done = !first_frame || gl_context_ready;
    if (init_done) sram_from_cart();
    if (gl_context_ready) {
        extern void wc_gl_release_cart_objects(void);
        wc_gl_release_cart_objects();
    }
    for (uint32_t i = 0; i < WC_MAX_PADS; i++) lr_rumble_stop(NULL, i);
    wc_host_exit_v8();
    wc_host_destroy(host);
    host = NULL;
    create_host();
    if (!host) return;
    lr_action_count = -1;   // input descriptors are re-sent for the new instance
    for (int p = 0; p < WC_LR_PORTS; p++) lr_device_sent[p] = -1;
    lr_frames = 0;
    frame_count = 0;
    time_ms = 0;
    if (!load_cart()) {
        notify("Restart failed: the cart could not be loaded again.", RETRO_LOG_ERROR, 10000);
        wc_host_destroy(host);
        host = NULL;
        return;
    }
    wc_log("wasmcart: reset: cart loaded again (%ux%u requested)\n", pref_width, pref_height);
    in_reset = true;
    if (gl_context_ready) finish_cart_init();
    else first_frame = true;  // 2D cart in software: the first frame finishes it
    in_reset = false;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port < WC_LR_PORTS) lr_port_device[port] = device;
}

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) {
    (void)index; (void)enabled; (void)code;
}

// ─── Performance log (core option wasmcart_perf_log) ────────────────────────
// One line a second: fps; the mean and worst time inside retro_run, split
// into the cart's code (wasm, V8, the host's import glue) and its GL calls
// (the driver); the time between retro_run calls (the frontend: present,
// swap, vsync wait); the dt handed to the cart (a cart on a fixed step runs
// dt / 16.7 ms ticks in the frame); V8 garbage collection and heap; CPU used
// by the core's other threads (V8's background compiler, the driver's
// threads, audio) next to the main thread's. In the first 30 s every frame
// over 50 ms gets a line of its own.
static FILE* perf_file = NULL;
static bool perf_file_tried = false;
static double perf_t_start = 0, perf_sec_t0 = 0, perf_prev_exit = 0;
static struct {
    unsigned frames;
    double run_sum, run_max, gl_sum, gap_sum, gap_max, dt_sum, dt_max, gc_ms, slowest_gl_ms;
    unsigned gl_calls, gc_count;
    const char* slowest_gl;
    // WebGPU carts
    double w_cart, w_queue, w_wait, w_wait_max, w_upload, w_latency;
    unsigned w_shown;
    // the frontend's callbacks (fe_t)
    double fe_video, fe_video_max, fe_audio, fe_audio_max, fe_input, samples;
    double cpu_main0, cpu_all0;
    // phases (ph_t), and inside wc_host_run_frame: the cart's wc_render, and
    // the WebGPU calls served by Dawn directly
    double p_input, p_glstate, p_frame, p_present, p_audio, p_render, p_host_frame;
    double wgpu_calls;
} PS;
/* CPU time of the calling thread (1) or the whole process (0); 0 where the clocks are missing */
static double cpu_ms(int thread) {
#if defined(CLOCK_THREAD_CPUTIME_ID) && defined(CLOCK_PROCESS_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(thread ? CLOCK_THREAD_CPUTIME_ID : CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec * 1e-6;
#else
    (void)thread;
    return 0;
#endif
}
static void perf_line(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_cb) log_cb(RETRO_LOG_INFO, "%s\n", buf);
    if (!perf_file_tried) {
        perf_file_tried = true;
        const char* dir = NULL;
        char path[1024];
        if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir)
            snprintf(path, sizeof path, "%s/wasmcart-perf.log", dir);
        else snprintf(path, sizeof path, "wasmcart-perf.log");
        perf_file = fopen(path, "a");
        if (log_cb) log_cb(RETRO_LOG_INFO, "wasmcart perf: writing %s%s\n", path, perf_file ? "" : " FAILED");
        if (perf_file) fprintf(perf_file, "wasmcart perf: new session\n");
    }
    if (perf_file) {
        fprintf(perf_file, "%s\n", buf);
        fflush(perf_file);
    }
}
// The lines for the frames since the last ones: once a second, and at unload
// for the last part second (a short session still gets its lines).
static double perf_heap_mb = 0;  // V8's heap at the last take
static void perf_emit(double t_exit, double t) {
    double secs = (t_exit - perf_sec_t0) / 1000.0, n = PS.frames ? PS.frames : 1;
    double cm = cpu_ms(1), ca = cpu_ms(0);
    double main_cpu = cm - PS.cpu_main0, bg_cpu = (ca - PS.cpu_all0) - main_cpu;
    perf_line("wasmcart perf: t=%.0f fps %.1f | run %.1f max %.1f | code %.1f gl %.1f (%.0f calls, slowest %s %.1f) | "
              "gap %.1f max %.1f | dt %.1f max %.1f ticks %.2f | gc %.1f ms (%u) heap %.1f MB | cpu main %.0f bg %.0f ms/s",
              t, PS.frames / secs, PS.run_sum / n, PS.run_max, (PS.run_sum - PS.gl_sum) / n, PS.gl_sum / n, PS.gl_calls / n,
              PS.slowest_gl ? PS.slowest_gl : "-", PS.slowest_gl_ms, PS.gap_sum / n, PS.gap_max, PS.dt_sum / n, PS.dt_max,
              PS.dt_sum / n / (1000.0 / 60.0), PS.gc_ms, PS.gc_count, perf_heap_mb, main_cpu / secs, bg_cpu / secs);
    {
        const wc_cart_info_t* ci = host ? wc_host_get_cart_info(host) : NULL;
        double rate = ci && ci->audio_sample_rate ? ci->audio_sample_rate : 48000.0;
        perf_line("wasmcart perf: t=%.0f frontend calls | video_cb %.1f max %.1f | audio_batch %.1f max %.1f "
                  "(%.0f samples a frame, %.0f a frame at %.0f Hz / 60) | input_poll %.1f",
                  t, PS.fe_video / n, PS.fe_video_max, PS.fe_audio / n, PS.fe_audio_max, PS.samples / n,
                  rate / 60.0, rate, PS.fe_input / n);
    }
    // the phases, in ms per frame; wc_render's GL imports are its "gl" share
    perf_line("wasmcart perf: t=%.0f phases | input %.3f | glstate %.3f | wc_render %.3f | gl_imports %.3f | "
              "host_frame %.3f | present %.3f | audio %.3f | other %.3f | wgpu_direct_calls %.0f",
              t, PS.p_input / n, PS.p_glstate / n, PS.p_render / n, PS.gl_sum / n, PS.p_host_frame / n,
              PS.p_present / n, PS.p_audio / n,
              (PS.run_sum - PS.p_input - PS.p_glstate - PS.p_frame - PS.p_present - PS.p_audio) / n, PS.wgpu_calls / n);
    if (host && wc_host_uses_wgpu(host)) {
        double sh = PS.w_shown ? PS.w_shown : 1;
        if (wgpu_gl_shared)
            perf_line("wasmcart perf: t=%.0f webgpu shared | cart %.1f (wc_render + glue) | copy %.1f | draw %.1f | shown %u of %u frames",
                      t, PS.w_cart / n, PS.w_queue / n, PS.w_upload / sh, PS.w_shown, PS.frames);
        else
            perf_line("wasmcart perf: t=%.0f webgpu readback | cart %.1f (wc_render + glue) | queue %.1f | wait %.1f max %.1f | "
                      "upload %.1f | latency %.1f (queue to mapped: GPU frame + copy) | shown %u of %u frames",
                      t, PS.w_cart / n, PS.w_queue / n, PS.w_wait / n, PS.w_wait_max, PS.w_upload / sh, PS.w_latency / sh,
                      PS.w_shown, PS.frames);
    }
    memset(&PS, 0, sizeof PS);
    PS.cpu_main0 = cm;
    PS.cpu_all0 = ca;
    perf_sec_t0 = t_exit;
}
static void perf_frame(double t_enter, double t_exit, double delta_ms) {
    static bool started = false;
    if (!started) {
        started = true;
        wc_host_prof_enable(1);
        wc_host_prof_t z;
        wc_host_prof_take(&z);
        perf_t_start = perf_sec_t0 = t_enter;
        perf_prev_exit = t_enter;
        PS.cpu_main0 = cpu_ms(1);
        PS.cpu_all0 = cpu_ms(0);
        perf_line("wasmcart perf: log on (t = seconds since the first frame; ms per frame: run = inside retro_run, "
                  "gl = the cart's GL calls, code = run - gl (wasm, V8, host glue), gap = between retro_run calls, "
                  "dt = the time handed to the cart, ticks = dt / 16.7 ms; bg = CPU ms per second of the core's other threads)");
    }
    wc_host_prof_t pr;
    wc_host_prof_take(&pr);
    perf_heap_mb = pr.heap_used_mb;
    double run = t_exit - t_enter, gap = t_enter - perf_prev_exit;
    perf_prev_exit = t_exit;
    PS.frames++;
    PS.run_sum += run;
    if (run > PS.run_max) PS.run_max = run;
    PS.gl_sum += pr.gl_ms;
    PS.gl_calls += pr.gl_calls;
    if (pr.gl_max_ms > PS.slowest_gl_ms) { PS.slowest_gl_ms = pr.gl_max_ms; PS.slowest_gl = pr.gl_max_name; }
    PS.gap_sum += gap;
    if (gap > PS.gap_max) PS.gap_max = gap;
    PS.dt_sum += delta_ms;
    if (delta_ms > PS.dt_max) PS.dt_max = delta_ms;
    PS.gc_ms += pr.gc_ms;
    PS.gc_count += pr.gc_count;
    PS.p_input += ph_t.input;
    PS.p_glstate += ph_t.glstate;
    PS.p_frame += ph_t.frame;
    PS.p_present += ph_t.present;
    PS.p_audio += ph_t.audio;
    PS.p_render += pr.render_ms;
    PS.p_host_frame += pr.frame_ms - pr.render_ms;
    PS.wgpu_calls += (double)pr.wgpu_calls;
    PS.fe_video += fe_t.video;
    if (fe_t.video > PS.fe_video_max) PS.fe_video_max = fe_t.video;
    PS.fe_audio += fe_t.audio;
    if (fe_t.audio > PS.fe_audio_max) PS.fe_audio_max = fe_t.audio;
    PS.fe_input += fe_t.input;
    PS.samples += fe_t.samples;
    if (host && wc_host_uses_wgpu(host)) {
        PS.w_cart += wgpu_t.cart;
        PS.w_queue += wgpu_t.queue;
        PS.w_wait += wgpu_t.wait;
        if (wgpu_t.wait > PS.w_wait_max) PS.w_wait_max = wgpu_t.wait;
        if (wgpu_t.shown) { PS.w_shown++; PS.w_upload += wgpu_t.upload; PS.w_latency += wgpu_t.latency; }
    }
    double t = (t_enter - perf_t_start) / 1000.0;
    if (t < 30 && run + gap > 50)
        perf_line("wasmcart perf: slow frame t=%.2f frame %u: run %.1f (gl %.1f in %u calls, slowest %s %.1f) gap %.1f dt %.1f gc %.1f (%u)",
                  t, frame_count, run, pr.gl_ms, pr.gl_calls, pr.gl_max_name ? pr.gl_max_name : "-", pr.gl_max_ms, gap, delta_ms,
                  pr.gc_ms, pr.gc_count);
    if (t_exit - perf_sec_t0 >= 1000.0) perf_emit(t_exit, t);
}

static void perf_flush(void) {
    if (!perf_log_on || !PS.frames) return;
    perf_emit(perf_prev_exit, (perf_prev_exit - perf_t_start) / 1000.0);
}
