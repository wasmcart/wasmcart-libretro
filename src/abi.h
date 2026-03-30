// abi.h — wasmcart ABI struct layout offsets (matches abi.js)

#ifndef WC_ABI_H
#define WC_ABI_H

#include <stdint.h>

// ─── wc_info_t field offsets (byte offsets from struct start) ───────────────
// Returned by wc_get_info(). All fields are u32.

#define WC_INFO_VERSION        0
#define WC_INFO_WIDTH          4
#define WC_INFO_HEIGHT         8
#define WC_INFO_FB_PTR         12
#define WC_INFO_AUDIO_PTR      16
#define WC_INFO_AUDIO_CAP      20   // capacity in stereo frames
#define WC_INFO_AUDIO_WRITE    24   // pointer to u32 write cursor in cart memory
#define WC_INFO_INPUT_PTR      28
#define WC_INFO_SAVE_PTR       32
#define WC_INFO_SAVE_SIZE      36
#define WC_INFO_TIME_PTR       40
#define WC_INFO_HOST_INFO_PTR  44
#define WC_INFO_STRUCT_SIZE    48

// Extended fields (v3)
#define WC_INFO_FLAGS          48   // u32 index 12
#define WC_INFO_AUDIO_RATE     52   // u32 index 13
#define WC_INFO_POINTER_PTR    56   // u32 index 14
#define WC_INFO_KEYS_PTR       60   // u32 index 15
#define WC_INFO_GPU_API        64   // u32 index 16 — 0=2D, 1=WebGL2/GLES3, 2=WebGPU, 3=Vulkan

// ─── wc_host_info_t field offsets (written by host before wc_init) ──────────

#define WC_HOST_INFO_PREFERRED_WIDTH   0
#define WC_HOST_INFO_PREFERRED_HEIGHT  4
#define WC_HOST_INFO_HOST_FPS          8
#define WC_HOST_INFO_AUDIO_SAMPLE_RATE 12
#define WC_HOST_INFO_FLAGS             16
#define WC_HOST_INFO_SIZE              20

// ─── wc_time_t layout (20 bytes) ───────────────────────────────────────────
// f64 time_ms   (offset 0)
// f64 delta_ms  (offset 8)
// u32 frame     (offset 16)

#define WC_TIME_TIME_MS   0
#define WC_TIME_DELTA_MS  8
#define WC_TIME_FRAME     16

// ─── Helper to read u32 from WASM memory ───────────────────────────────────

static inline uint32_t wc_read_u32(const uint8_t* mem, uint32_t offset) {
    return *(const uint32_t*)(mem + offset);
}

static inline void wc_write_u32(uint8_t* mem, uint32_t offset, uint32_t val) {
    *(uint32_t*)(mem + offset) = val;
}

static inline void wc_write_f64(uint8_t* mem, uint32_t offset, double val) {
    *(double*)(mem + offset) = val;
}

#endif // WC_ABI_H
