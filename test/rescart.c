/*
 * rescart -- fixture for Restart Content (retro_reset) in test/run-tests.sh:
 * a GL cart that renders at the host's preferred size and counts its runs in
 * its save region, as savecart does.
 *
 * wc_init reads wc_host_info_t's preferred_width/height and declares that
 * size (64x64 without one), reads the run count from the save region (the
 * host loaded SAVE_RAM before wc_init), keeps it as `seen` and writes
 * seen + 1 back, and makes a texture (a GL object a restart must release).
 * Every frame clears its whole size to the colour of what wc_init read:
 * seen 0 red (ff2222), 1 green (22ff22), 2 blue (2222ff), 3+ white.
 *
 * Built by test/run-tests.sh with wasi-sdk; there is no libc.
 */
typedef unsigned int u32;

#define EXPORT(n) __attribute__((export_name(n)))
#define GLIMPORT(n) __attribute__((import_module("gl"), import_name(n)))

GLIMPORT("glClear") void glClear(u32 mask);
GLIMPORT("glClearColor") void glClearColor(float r, float g, float b, float a);
GLIMPORT("glViewport") void glViewport(int x, int y, int w, int h);
GLIMPORT("glGenTextures") void glGenTextures(u32 n, u32* out);
GLIMPORT("glBindFramebuffer") void glBindFramebuffer(u32 target, u32 fb);

#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_BUFFER_BIT 0x4000

static struct {
    u32 version, width, height, fb_ptr, audio_ptr, audio_cap, audio_write_ptr,
        input_ptr, save_ptr, save_size, time_ptr, host_info_ptr, flags,
        audio_sample_rate, pointer_ptr, keys_ptr, gpu_api, wheel_ptr;
} info;
static struct { u32 preferred_width, preferred_height, reserved, audio_sample_rate, flags; } host_info;
static u32 pads[4 * 8];
static u32 time_info[8];
static u32 save[4];
static u32 seen, tex;

EXPORT("wc_get_info") u32 wc_get_info(void) {
    info.version = 4;
    if (!info.width) { info.width = 64; info.height = 64; }
    info.input_ptr = (u32)pads;
    info.save_ptr = (u32)save;
    info.save_size = sizeof save;
    info.time_ptr = (u32)time_info;
    info.host_info_ptr = (u32)&host_info;
    info.gpu_api = 1;
    return (u32)&info;
}

EXPORT("wc_init") void wc_init(void) {
    if (host_info.preferred_width && host_info.preferred_height) {
        info.width = host_info.preferred_width;
        info.height = host_info.preferred_height;
    }
    seen = save[0];
    save[0] = seen + 1;
    glGenTextures(1, &tex);
}

EXPORT("wc_render") void wc_render(void) {
    static const float colour[4][3] = { { 1, 0.133f, 0.133f }, { 0.133f, 1, 0.133f }, { 0.133f, 0.133f, 1 }, { 1, 1, 1 } };
    const float* c = colour[seen < 3 ? seen : 3];
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (int)info.width, (int)info.height);
    glClearColor(c[0], c[1], c[2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
}
