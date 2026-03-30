// egl_context.h — EGL context management for standalone player

#ifndef WC_EGL_CONTEXT_H
#define WC_EGL_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

// Create an EGL context with a pbuffer surface (headless, for GL carts).
// Must be called BEFORE SDL window creation.
// Returns 0 on success.
int egl_create_context(uint32_t width, uint32_t height);

// Create a window surface from an SDL window's native handle.
// Call after SDL_CreateWindow.
int egl_create_window_surface(void* native_window);

// Make the EGL context current (call after SDL init to re-assert).
void egl_make_current(void);

// Swap buffers (present GL frame).
void egl_swap_buffers(void);

// Destroy EGL context and surfaces.
void egl_destroy(void);

// Get a GL function pointer by name (for wc_host_set_gl_loader).
void* egl_get_proc_address(const char* name);

// Is EGL initialized?
bool egl_is_initialized(void);

#endif // WC_EGL_CONTEXT_H
