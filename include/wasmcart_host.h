// wasmcart_host.h — Native C CartHost public API
// Embed wasmcart in any application: standalone player, libretro core, etc.

#ifndef WASMCART_HOST_H
#define WASMCART_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── ABI Constants (matches abi.js) ────────────────────────────────────────

#define WC_ABI_VERSION     3
#define WC_MIN_ABI_VERSION 1

#define WC_MAX_PADS        4
#define WC_PAD_SIZE        16
#define WC_TIME_SIZE       20
#define WC_MAX_POINTERS    10
#define WC_KEYS_STATE_SIZE 32

// Button bitmask
#define WC_BUTTON_A      (1 << 0)
#define WC_BUTTON_B      (1 << 1)
#define WC_BUTTON_X      (1 << 2)
#define WC_BUTTON_Y      (1 << 3)
#define WC_BUTTON_L      (1 << 4)
#define WC_BUTTON_R      (1 << 5)
#define WC_BUTTON_START  (1 << 6)
#define WC_BUTTON_SELECT (1 << 7)
#define WC_BUTTON_UP     (1 << 8)
#define WC_BUTTON_DOWN   (1 << 9)
#define WC_BUTTON_LEFT   (1 << 10)
#define WC_BUTTON_RIGHT  (1 << 11)
#define WC_BUTTON_L3     (1 << 12)
#define WC_BUTTON_R3     (1 << 13)

// Cart info flags
#define WC_FLAG_AUDIO_F32 (1 << 0)
#define WC_FLAG_NET_WS    (1 << 1)
#define WC_FLAG_NET_DC    (1 << 2)
#define WC_FLAG_POINTER   (1 << 3)
#define WC_FLAG_KEYBOARD  (1 << 4)

// ─── Structs ───────────────────────────────────────────────────────────────

// Matches the in-memory layout of wc_pad_t (16 bytes)
typedef struct {
    uint16_t buttons;
    int16_t  left_x;
    int16_t  left_y;
    int16_t  right_x;
    int16_t  right_y;
    uint8_t  left_trigger;
    uint8_t  right_trigger;
    uint8_t  connected;
    uint8_t  _pad;
} wc_pad_t;

// Parsed from wc_get_info() return
typedef struct {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fb_ptr;
    uint32_t audio_ptr;
    uint32_t audio_cap;       // capacity in stereo frames
    uint32_t audio_write_ptr; // pointer to cart's write cursor (u32 in cart memory)
    uint32_t input_ptr;
    uint32_t save_ptr;
    uint32_t save_size;
    uint32_t time_ptr;
    uint32_t host_info_ptr;
    uint32_t flags;
    uint32_t audio_sample_rate;
    // v3 extended
    uint32_t pointer_ptr;
    uint32_t keys_ptr;
    uint32_t gpu_api;          // 0=2D, 1=WebGL2/GLES3, 2=WebGPU, 3=Vulkan
} wc_cart_info_t;

// GPU API values
#define WC_GPU_API_NONE    0
#define WC_GPU_API_WEBGL2  1
#define WC_GPU_API_WEBGPU  2
#define WC_GPU_API_VULKAN  3

// Parsed from manifest.json
typedef struct {
    char     name[256];
    char     version[64];
    uint32_t abi;
    char     entry[256];
    uint32_t players;
    bool     pointer;
    bool     keyboard;
    // networking (v3)
    bool     websocket;
    bool     data_channel;
    char     ws_domains[8][256]; // up to 8 allowed domains
    uint32_t ws_domain_count;
} wc_manifest_t;

// Options for loading a cart
typedef struct {
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t host_fps;
    uint32_t audio_sample_rate;
    const uint8_t* save_data;
    uint32_t save_data_size;
    bool     defer_init;        // if true, skip _initialize/wc_init — call wc_host_finish_init() later
} wc_host_options_t;

// ─── Host API ──────────────────────────────────────────────────────────────

typedef struct wc_host wc_host_t;

// Lifecycle
wc_host_t* wc_host_create(void);
void       wc_host_destroy(wc_host_t* host);

// Loading
int  wc_host_load_file(wc_host_t* host, const char* wasc_path, const wc_host_options_t* opts);
int  wc_host_load_memory(wc_host_t* host, const uint8_t* data, size_t len, const wc_host_options_t* opts);
int  wc_host_finish_init(wc_host_t* host);  // call after GL context ready if defer_init was set

// Input — call before wc_host_run_frame()
void wc_host_set_pads(wc_host_t* host, const wc_pad_t pads[WC_MAX_PADS]);
void wc_host_set_keyboard(wc_host_t* host, const uint8_t keys[WC_KEYS_STATE_SIZE]);
void wc_host_set_pointer(wc_host_t* host, int index, int16_t x, int16_t y, uint8_t buttons, uint8_t active);
void wc_host_set_time(wc_host_t* host, double time_ms, double delta_ms, uint32_t frame);

// Run one frame — calls wc_render() on the cart
void wc_host_run_frame(wc_host_t* host);
void wc_host_enter_v8(void);
void wc_host_exit_v8(void);

// Readback — call after wc_host_run_frame()
const uint8_t*  wc_host_get_framebuffer(wc_host_t* host, uint32_t* width, uint32_t* height);
const void*     wc_host_get_audio(wc_host_t* host, uint32_t* num_frames, bool* is_f32);
uint8_t*        wc_host_get_save_data(wc_host_t* host, uint32_t* size);

// GL carts
bool wc_host_uses_gl(wc_host_t* host);
// Set a function that resolves GL function names to pointers.
// For standalone: use eglGetProcAddress. For libretro: use retro_hw_get_proc_address.
typedef void* (*wc_gl_get_proc_fn)(const char* name);
void wc_host_set_gl_loader(wc_host_t* host, wc_gl_get_proc_fn loader);

// Error state
bool wc_host_has_trapped(wc_host_t* host);

// Info
const wc_cart_info_t* wc_host_get_cart_info(wc_host_t* host);
const wc_manifest_t*  wc_host_get_manifest(wc_host_t* host);

// Direct memory access (for save states, etc.)
void*  wc_host_get_memory(wc_host_t* host, uint32_t* size);

#ifdef __cplusplus
}
#endif

#endif // WASMCART_HOST_H
