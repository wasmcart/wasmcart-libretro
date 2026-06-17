# wasmcart-libretro — dev notes (hard-won gotchas)

wasmcart-libretro is the **libretro core** that runs wasmcart carts (WebAssembly
games) inside RetroArch. It is the sibling of **wasmcart-native** (the standalone
host that owns its own window + EGL context) — the same relationship as
jsgame-libretro ↔ jsgamelauncher. They share cart-runtime code (`wasmcart-native`
is a submodule) but are **built differently**: the libretro core uses RetroArch as
its host; the native build is its own host.

## 0. What a libretro core bakes in vs. what RetroArch supplies

**The core statically links ONLY what RetroArch does NOT provide:**
- **libnode** — V8 + the WASM engine that runs the cart
- **webaudio C** — the DSP/audio engine
- **Rust canvas + Ganesh/Skia** — 2D + GPU-accelerated canvas

**Everything else comes FROM RetroArch (the common host):**
- the **GL context** + `get_proc_address` (GPU access)
- **input** (RetroPad → the cart), **audio output** sink, the **window**, the
  **save files**, the **frame clock**

So the core links **no GL/EGL/windowing library of its own**. It must be a single
statically-linked `wasmcart_libretro.<so|dll|dylib>` — that *is* what a libretro
core is, and what the RetroArch Core Downloader requires (one file per core).

## 1. The core links NO GL library — load ALL GL from RetroArch's get_proc_address

**This is the most important lesson, and it cost real Android debugging time.**

The original core called ~170 GL functions DIRECTLY against `<GLES3/gl3.h>` (via
the shared `wasmcart-native/src/gl_imports.cpp`, which the core compiles in), so
the `.so` carried undefined `gl*` symbols resolved against a GL library:
- Linux/Android: system/NDK `libGLESv2` satisfied them.
- macOS/Windows: no system GLES → pulled in **ANGLE** → shipped `libEGL`/
  `libGLESv2` **sidecar dylibs** → NOT single-file → not Core-Downloader-shippable.

Worse, the core *also* requested RetroArch's `hw_render` context but ignored its
`get_proc_address` ("Not needed for basic operation"), and the native side even
created its OWN EGL context. **On Android this fights the frontend's GLES context**
— wrong display/context, hours of "why won't it render on this device." The root
cause was never using the GPU interface RetroArch hands every core.

**The fix:**
- `wasmcart-native/src/gl_procs.{h,c}`: a `p_glXxx` pointer per GL function, with
  `#define glXxx p_glXxx` (after the GLES headers). `wc_gl_procs_load(get_proc)`
  populates them.
- The loader is fed from `host->gl_loader` (a field that already existed but was
  stored-and-never-used). `wc_gl_setup_redirect` calls `wc_gl_procs_load(
  host->gl_loader)` on context_reset, before any GL call.
- **The libretro core** sets that loader in `on_context_reset`:
  `wc_host_set_gl_loader(host, (wc_gl_get_proc_fn)hw_render.get_proc_address)`.
  **The native host** sets it to `egl_get_proc_address` (it always did). Same
  mechanism, two hosts.
- `src/libretro.c` itself also reads GL state directly (cart GL save/restore), so
  it includes `gl_procs.h` too.

Result: the core links **no GL library on any platform** — single file everywhere,
no ANGLE.

**Verify after any GL change (regression guard):**
```
nm -D build/wasmcart_libretro.so | grep -E ' U (gl[A-Z]|egl)'   # must be EMPTY
ldd build/wasmcart_libretro.so | grep -iE 'GLES|EGL'            # must be EMPTY
```

**Gotchas:**
- Generate the GL function list from what the LINKER reports undefined
  (`nm -D ... U gl*`) — NOT a source grep (underscored names like
  `glGetIntegeri_v` need `gl[A-Z][a-zA-Z0-9_]*`).
- `gl_procs.h` must `#include <GLES3/gl3.h>`, `gl31.h`, AND `gl32.h` itself, so
  every PFN typedef resolves even for a file that only included `gl3.h` (e.g.
  `libretro.c`). Several entry points (`PFNGLBLENDBARRIERPROC`,
  `PFNGLMEMORYBARRIERPROC`, `PFNGLTEXBUFFERPROC`, …) live in gl31/gl32.
- The desktop-GL-only functions the cart import table exposes
  (`glBindFragDataLocation`, `glTextureBarrier`, `glMultiDrawArraysIndirect`, …)
  are **no-op stubs** that never call GL — so the core needs only **core GLES3**
  (Android-consistent). Don't link desktop GL to satisfy them.
- Android: the core has **no Skia of its own** in the libretro build, so it links
  **no** GL (removed the old `GLESv3 EGL`). (Contrast jsgame-libretro, whose core
  DOES bundle Skia and so links Android's *system* GLESv3/EGL for Skia — that's
  fine, they're system libs, single-file-safe.)
- Vendor the GLES3 headers (`wasmcart-native/vendor/gl`, Khronos/MIT) so
  macOS/Windows build without ANGLE headers.

**Build is two repos.** The GL changes live in `wasmcart-native` (the submodule);
the core wires the loader + removes ANGLE in `wasmcart-libretro`. After editing
native, you must bump the submodule pointer in the core (`git add
deps/wasmcart-native`) or the core builds against the old native.

## 2. Security: the wasm import boundary is tighter than a JS realm

wasmcart's sandbox story is **stronger than jsgame-libretro's**, by construction:

- **The cart is WebAssembly.** It is sandboxed by V8's wasm engine and can reach
  the host ONLY through an explicit, curated **import table** — literally the
  `GL_REG`/`GL_E(...)` entries in `gl_imports.cpp` plus the other registered host
  functions. The cart cannot call anything you did not put in that list. This is
  **capability-based**: the entire host surface a cart can touch is one auditable
  table you can read top to bottom.
- **jsgame-libretro runs JS in a `node:vm` realm.** Strong (no process/require/fs),
  but it's **removal-based** sandboxing over a huge, dynamic surface (all of JS +
  the browser API), where security depends on having stripped everything
  dangerous. A realm escape is a known class of risk.

So wasmcart's attack surface is *exactly* the import table — narrower and more
defensible than "JS locked in a realm." When adding a host import, remember you
are widening the cart's capability set; keep the table minimal and reviewed.

## 3. GL state save/restore around the cart (FBO redirect)

The core renders the cart into a redirect FBO (depth+stencil, since Three.js needs
depth and Ganesh needs stencil — RetroArch's hw_render FBO may lack them) and
blits to the frontend. Because the cart and RetroArch share one GL context, the
core **saves the cart's GL state** (`save_cart_gl_state` in `libretro.c`) and
restores it each frame so neither corrupts the other's bindings. These reads are GL
calls — they go through the `get_proc_address`-loaded pointers like everything else
(don't reintroduce direct `glXxx` here).
EOF
echo "created wasmcart-libretro/doc/dev_notes.md ($(wc -l < /home/monteslu/code/cliemu/wasmcart-libretro/doc/dev_notes.md) lines)"
ls -la /home/monteslu/code/cliemu/wasmcart-libretro/doc/dev_notes.md