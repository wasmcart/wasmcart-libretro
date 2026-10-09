/*
 * allocart -- fixture for test/core_test.c: a GL cart that does (or does not)
 * reach the host paths that write into cart memory, built in one of several
 * allocator shapes (SPEC.md, "Cart memory the host writes").
 *
 *   -DMODE_WCALLOC   exports wc_alloc/wc_free; asks for GL strings
 *   -DMODE_QUIET     exports no allocator; never asks for a string
 *   -DMODE_NONE_HIT  exports no allocator; asks for a GL string
 *   -DMODE_MALLOC    exports only malloc (the deprecated fallback)
 *   -DMODE_BAD_OOB   wc_alloc answers a pointer past the end of memory
 *   -DMODE_BAD_ZERO  wc_alloc answers 0 (out of memory)
 *   -DMODE_MISALIGN  wc_alloc ignores alignment (fine for strings, which ask
 *                    for 1; wrong for glMapBufferRange, which asks for 16)
 *
 * Frame 2 asks for GL strings; frame 3 maps a buffer twice (write, then read
 * back what the first unmap wrote). The allocator shapes that can satisfy
 * both report ok=1 after frame 3.
 *
 * The cart reports through its save region, which the libretro core hands
 * the frontend as RETRO_MEMORY_SAVE_RAM: a live view of cart memory.
 * test/run.sh builds it with wasi-sdk; there is no libc.
 */
typedef unsigned int u32;
typedef unsigned char u8;

#define EXPORT(n) __attribute__((export_name(n)))
#define GLIMPORT(n) __attribute__((import_module("gl"), import_name(n)))

GLIMPORT("glGetString") u32 glGetString(u32 name);
GLIMPORT("glClear") void glClear(u32 mask);
GLIMPORT("glClearColor") void glClearColor(float r, float g, float b, float a);
GLIMPORT("glGenBuffers") void glGenBuffers(u32 n, u32* out);
GLIMPORT("glBindBuffer") void glBindBuffer(u32 target, u32 buf);
GLIMPORT("glBufferData") void glBufferData(u32 target, u32 size, const void* data, u32 usage);
GLIMPORT("glMapBufferRange") u32 glMapBufferRange(u32 target, u32 offset, u32 length, u32 access);
GLIMPORT("glUnmapBuffer") u32 glUnmapBuffer(u32 target);

#define GL_ARRAY_BUFFER 0x8892
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_MAP_READ_BIT  0x1
#define GL_MAP_WRITE_BIT 0x2

#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02

static struct {
    u32 magic;     /* 0xA110CA7E once wc_init ran */
    u32 frames;    /* wc_render calls */
    u32 ok;        /* 1: every string check passed; 2: a check failed */
    u32 allocs;    /* wc_alloc / malloc calls */
    u32 frees;     /* wc_free calls */
    u32 str_version, str_version2, str_renderer;
    u32 map1, map2;
} save;

static struct {
    u32 version, width, height, fb_ptr, audio_ptr, audio_cap, audio_write_ptr,
        input_ptr, save_ptr, save_size, time_ptr, host_info_ptr, flags,
        audio_sample_rate, pointer_ptr, keys_ptr, gpu_api, wheel_ptr;
} info;
static u32 pads[4 * 8];
static u32 time_info[8];

#if defined(MODE_WCALLOC) || defined(MODE_MALLOC) || defined(MODE_MISALIGN)
static u8 arena[4096] __attribute__((aligned(16)));
static u32 arena_top;
static u32 bump(u32 size, u32 align) {
    save.allocs++;
    if (align < 1) align = 1;
    u32 base = ((u32)arena + arena_top + align - 1) & ~(align - 1);
    if (base + size > (u32)arena + sizeof arena) return 0;
    arena_top = base + size - (u32)arena;
    return base;
}
static int in_arena(u32 p) { return p >= (u32)arena && p < (u32)arena + sizeof arena; }
#else
static int in_arena(u32 p) { (void)p; return 1; }
#endif

#ifdef MODE_WCALLOC
EXPORT("wc_alloc") u32 wc_alloc(u32 size, u32 align) { return bump(size, align); }
EXPORT("wc_free") void wc_free(u32 ptr) { (void)ptr; save.frees++; }
#endif
#ifdef MODE_MALLOC
/* 16, like wasi-libc and emscripten mallocs (alignof(max_align_t)): the host
   asks for 16 for a buffer mapping, and a malloc fallback has no way to ask. */
EXPORT("malloc") void* malloc(unsigned long size) { return (void*)bump((u32)size, 16); }
#endif
#ifdef MODE_MISALIGN
EXPORT("wc_alloc") u32 wc_alloc(u32 size, u32 align) { u32 p = bump(size + 1, 1); return p ? (p | 1) : 0; (void)align; }
EXPORT("wc_free") void wc_free(u32 ptr) { (void)ptr; save.frees++; }
#endif
#ifdef MODE_BAD_OOB
EXPORT("wc_alloc") u32 wc_alloc(u32 size, u32 align) { (void)size; (void)align; save.allocs++; return 0xFFFFFF00u; }
EXPORT("wc_free") void wc_free(u32 ptr) { (void)ptr; save.frees++; }
#endif
#ifdef MODE_BAD_ZERO
EXPORT("wc_alloc") u32 wc_alloc(u32 size, u32 align) { (void)size; (void)align; save.allocs++; return 0; }
EXPORT("wc_free") void wc_free(u32 ptr) { (void)ptr; save.frees++; }
#endif

static int starts(u32 p, const char* want) {
    if (!p) return 0;
    const char* s = (const char*)p;
    while (*want) if (*s++ != *want++) return 0;
    return 1;
}

EXPORT("wc_get_info") u32 wc_get_info(void) {
    info.version = 4;
    info.width = 64;
    info.height = 64;
    info.input_ptr = (u32)pads;
    info.save_ptr = (u32)&save;
    info.save_size = sizeof save;
    info.time_ptr = (u32)time_info;
    info.gpu_api = 1;
    return (u32)&info;
}

EXPORT("wc_init") void wc_init(void) { save.magic = 0xA110CA7Eu; }

EXPORT("wc_render") void wc_render(void) {
    save.frames++;
#ifndef MODE_QUIET
    if (save.frames == 2) {
        save.str_version = glGetString(GL_VERSION);
        save.str_version2 = glGetString(GL_VERSION);
        save.str_renderer = glGetString(GL_RENDERER);
        int good = starts(save.str_version, "OpenGL ES 3.0")
                && save.str_version2 == save.str_version
                && save.str_renderer != 0 && save.str_renderer != save.str_version
                && in_arena(save.str_version) && in_arena(save.str_renderer);
        save.ok = good ? 1 : 2;
    }
    if (save.frames == 3 && save.ok == 1) {
        static u8 init[64];
        for (u32 i = 0; i < 64; i++) init[i] = (u8)i;
        u32 buf;
        glGenBuffers(1, &buf);
        glBindBuffer(GL_ARRAY_BUFFER, buf);
        glBufferData(GL_ARRAY_BUFFER, 64, init, GL_DYNAMIC_DRAW);
        int good = 1;
        save.map1 = glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
        u8* m = (u8*)save.map1;
        if (!m || (save.map1 & 15)) good = 0;
        else {
            for (u32 i = 0; i < 64; i++) { if (m[i] != (u8)i) good = 0; m[i] = (u8)(200 - i); }
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        if (good) {
            save.map2 = glMapBufferRange(GL_ARRAY_BUFFER, 0, 64, GL_MAP_READ_BIT);
            m = (u8*)save.map2;
            if (!m) good = 0;
            else {
                for (u32 i = 0; i < 64; i++) if (m[i] != (u8)(200 - i)) good = 0;
                glUnmapBuffer(GL_ARRAY_BUFFER);
            }
        }
        save.ok = good ? 1 : 3;
    }
#else
    save.ok = 1;
#endif
    glClearColor(0.f, 0.5f, 0.f, 1.f);
    glClear(0x4000);
}
