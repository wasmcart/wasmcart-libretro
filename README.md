# wasmcart-libretro

[RetroArch](https://www.retroarch.com) [libretro](https://www.libretro.com) core for
[wasmcart](https://github.com/wasmcart/wasmcart). Runs `.wasc` game carts in RetroArch
with full gamepad, audio, save state, shader, and netplay support. Powered by
[V8](https://v8.dev) via [libnode](https://github.com/wasmcart/build-libnode) for
instant WASM startup.

## Working Carts

| Cart | Type | Engine |
|------|------|--------|
| Snake | 2D framebuffer | C/[Emscripten](https://emscripten.org) |
| Three.js Demo | GL (WebGL2) | [Three.js](https://threejs.org) direct |
| Warlords | GL ([Godot](https://godotengine.org) 4.4) | Godot GLES3 Compatibility |
| RoboBlast TPS | GL (Godot 4.4) | Godot GLES3, 3D |
| OpenArena | GL ([ioquake3](https://github.com/ioquake/ioq3)) | renderergl2 GLES |

## Build

```bash
# 1. Clone with submodule
git clone --recursive https://github.com/wasmcart/wasmcart-libretro.git
cd wasmcart-libretro

# 2. Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v26.3.0-jsg9/libnode-linux-x86_64.tar.gz | tar xz -C deps/libnode

# 3. Build the core
mkdir build && cd build
cmake ..
make -j$(nproc)

# 4. Install
CORES=~/.var/app/org.libretro.RetroArch/config/retroarch/cores
cp wasmcart_libretro.so $CORES/
cp -r wasmcart_libretro_wgpu $CORES/   # if built with WebGPU (see "WebGPU carts")
cp ../wasmcart_libretro.info $CORES/
```

Pre-built libnode downloads for all platforms at [wasmcart/build-libnode releases](https://github.com/wasmcart/build-libnode/releases).

The core links **no GL library** — every GL entry point is loaded at runtime from
RetroArch's `get_proc_address` (the GLES3 *headers* are vendored in
`deps/wasmcart-native/vendor/gl`). No ANGLE, no `-DANGLE_DIR`, no sidecar libs: the
shipped core is a single self-contained `wasmcart_libretro.<ext>` on every platform.

### Test

```bash
test/run.sh            # Linux; needs wasi-sdk (WASI_SDK=...) and Mesa's EGL/GLES
```

`test/core_test.c` is a minimal libretro frontend: it loads the built core with
`dlopen`, gives it a GLES3 context on Mesa's surfaceless EGL display (no window),
and runs small carts built from `test/allocart.c`. They cover the memory the host
writes into a cart (SPEC.md, "Cart memory the host writes"): GL strings and
`glMapBufferRange` mappings in blocks from the cart's `wc_alloc`, the clear error
for a cart without `wc_alloc`/`wc_free` (one that exports only `malloc`, with or
without `free`, gets the same error: its `malloc` is never used), and rejected
pointers.

## Usage

```bash
# Flatpak RetroArch
flatpak run \
  --filesystem=/path/to/your/roms \
  org.libretro.RetroArch \
  -L ~/.var/app/org.libretro.RetroArch/config/retroarch/cores/wasmcart_libretro.so \
  /path/to/game.wasc

# Native RetroArch
retroarch -L /path/to/wasmcart_libretro.so /path/to/game.wasc
```

## Architecture

```
wasmcart_libretro.so (~76MB statically linked)
├── libretro.c              <- RetroArch <-> wasmcart bridge (only libretro-specific file)
├── deps/wasmcart-native/   <- git submodule (shared with wasmcart-native standalone host)
│   ├── src/cart_host.cpp   <- V8 WASM compile/instantiate/call
│   ├── src/gl_imports.cpp  <- ~200 GL functions as V8 FunctionCallbacks
│   ├── src/asset_loader.c  <- .wasc ZIP reading (miniz)
│   └── deps/               <- miniz, cJSON
└── deps/libnode/           <- Pre-built V8 runtime (download separately)
```

Shared code is pulled from [wasmcart-native](https://github.com/wasmcart/wasmcart-native) via git submodule. Changes to the GL import bridge, WASM host, or asset loader automatically apply to both the standalone player and the libretro core.

## GL Surface

The [wasmcart GL surface](https://github.com/wasmcart/wasmcart/blob/main/docs/gl-surface.md)
is **WebGL2 / OpenGL ES 3.0** — the same on all hosts. The host reports `GL_VERSION = "OpenGL ES 3.0 wasmcart"` regardless of the actual driver. Real driver extensions pass through for carts that need them (e.g., Godot texture format detection).

RetroArch provides a Core 3.3 GL context (via GLX). ES 3.0 shaders (`#version 300 es`) work on Core 3.3 via `GL_ARB_ES3_compatibility`. The FBO redirect intercepts `glBindFramebuffer(0)` → capture FBO with depth+stencil, then blits to RetroArch's hw_render FBO after each frame.

## WebGPU carts

wasmcart has an optional WebGPU tier ([docs/webgpu.md](https://github.com/wasmcart/wasmcart/blob/main/docs/webgpu.md)).
libretro has no WebGPU hardware context, so the core makes its own WebGPU device
through [Dawn](https://dawn.googlesource.com/dawn) (Vulkan on Linux), independent
of RetroArch's video driver, which stays `gl`. The cart renders on that device,
on the same GPU as RetroArch's GL context, and each frame is drawn into
RetroArch's GL framebuffer, so shaders, overlays, screenshots and recording work
as for a GL cart.

Dawn and the WebGPU host code are not inside the core: they are in a directory,
`wasmcart_libretro_wgpu/`, that must sit in the same directory as
`wasmcart_libretro.so`. If the core is reached through a symlink (Batocera links
`/usr/lib/libretro/wasmcart_libretro.so` to the real file under `/userdata`), the
directory goes beside the real file. `WASMCART_WGPU_DIR=<dir>` in the environment
points the core somewhere else. The directory holds `dawn.node` and
`libwebgpu_dawn.so` from [native-dawn](https://github.com/monteslu/native-dawn)
0.1.6, wasmcart-native's `wgpu_bridge.cjs`, and wasmcart's `src/wgpu` JavaScript.

Which GPU API a cart gets:

| Cart | `wasmcart_libretro_wgpu/` present and Dawn finds a Vulkan adapter | Otherwise |
|------|------|------|
| GL only | GL | GL |
| GL + WebGPU (dual) | WebGPU | GL (the log says why) |
| WebGPU only | WebGPU | refused at load, with the reason on screen |

On load the core shows which one a cart that imports WebGPU got (`WebGPU on
<adapter>`, or `GL (no WebGPU: <why>)`), and the log (RetroArch's, and
`wasmcart.log` beside the cart) says the same. The core option **GPU API** set
to GL, or `WASMCART_NO_WGPU=1` in the environment, makes the core act as a host
without WebGPU.

The GPU: once RetroArch's GL context exists, the core asks it which GPU it is
on (GL_EXT_memory_object's device UUID, matched against Vulkan's devices), and
if Dawn took another one (a machine with two GPUs) the cart is loaded again on
the GL context's GPU before it starts. The log says so.

The frame never leaves the GPU where the GL context allows it (Linux, a GL with
GL_EXT_memory_object_fd and GL_EXT_semaphore_fd, and GL and Vulkan from the
same driver: Mesa's radeonsi and RADV, for example): the core allocates the
frame's memory with Vulkan on that GPU and both Dawn and GL import it, Dawn
copies the cart's frame into it, and GL draws it into RetroArch's framebuffer
after waiting, on the GPU, for Dawn's copy (semaphores both ways). The picture
is the frame the cart just rendered, and the CPU never waits for the GPU. The
log says `WebGPU frames reach GL on the GPU, zero-copy`.

Otherwise the frame is read back, and the log and an on-screen message say
`WebGPU frames reach GL by READBACK` with the reason: a pipelined copy into a
mapped buffer (frame N's copy queued as soon as it is rendered, frame N-1
drawn from the mapped buffer), one frame behind the cart, about 8 MB each way
per frame at 1080p. A lost device (a driver reset) stops the cart with an
on-screen message; Restart Content starts it again.

Dawn's command validation is off (it costs CPU on every call;
`WASMCART_WGPU_VALIDATION=1` turns it on), and the calls a frame makes most go
from the cart straight to Dawn's C API (wasmcart-native `src/wgpu_imports.cpp`).

The WebGPU device follows the wasmcart WebGPU tier: the adapter is requested in
WebGPU compatibility mode (the tier's floor, so a cart cannot depend on more
than a GLES 3.1-class GPU has), as every wasmcart host does. A cart that asks
its adapter for info sees backend "WebGPU", type unknown and ids 0 (the
emdawnwebgpu glue reports that on every JavaScript host, browsers included);
the host log names the real GPU.

Platforms: the Linux x86_64 and aarch64 release tarballs include
`wasmcart_libretro_wgpu/`. The macOS, Windows and Android cores ship without
it and run carts on GL only (dual carts on GL; WebGPU-only carts are refused).

Building it: point CMake at native-dawn's release archive for the platform and at
wasmcart's `src/wgpu`; `deps/wasmcart-native/scripts/fetch-wgpu-deps.sh` fetches both
(native-dawn by pinned sha256):

```bash
NATIVE_DAWN_VERSION=0.1.6 WASMCART_VERSION=0.35.1 \
  sh deps/wasmcart-native/scripts/fetch-wgpu-deps.sh linux-x64 deps/wgpu
cmake -B build -DWASMCART_WGPU_JS_DIR=$PWD/deps/wgpu/wasmcart/src/wgpu \
               -DNATIVE_DAWN_DIR=$PWD/deps/wgpu/native-dawn
cmake --build build    # build/wasmcart_libretro.so + build/wasmcart_libretro_wgpu/
```

## Controllers

Ports 1-4 are the cart's pads 0-3 (players 1-4), as RetroPads. libretro does
not tell a core whether a port has a controller behind it, so port 1 is always
connected and ports 2-4 become connected the first time they give any input
(a button, or a stick or trigger past a quarter of its travel), the way a
browser reveals a gamepad on its first press. A port set to None in Quick Menu
→ Controls, or past the number of users RetroArch is set to, is disconnected.
`test/pads_test.sh` checks it with wasmcart-native's pad cart.

## No content: the test cart

Load Core, then Start Core (no content), runs the test cart built into the core
(wasmcart's [`testcart/`](https://github.com/wasmcart/wasmcart/tree/main/testcart);
`testcart/` here has the copy the core compiles in). It checks a setup by eye:

- the four pads, every button lit while held, sticks, triggers, raw values and
  the device; L3 and R3 run the rumble motors
- the pointer (mouse and touch slots, with trails) and the keyboard
- sound: A, B, X and Y each play their own note (P1 sine, P2 square, P3
  triangle, P4 saw); holding L sends them to the left channel only and R to the
  right only; the triggers bend the pitch
- saves: each start checks the save byte for byte and says FRESH, OK (with the
  start count), TRUNCATED or CORRUPT; SELECT+START held 3 seconds clears it, and
  the next start must say FRESH

The cart is written to `wasmcart/testcart.wasc` in the save directory and runs
from there like any cart; its save is the core's `.srm`.

## Core Options

- **Internal Resolution**: 270p, 360p, 640x480, 800x480, 540p, 1024x768, 720p, 1280x800, 1080p, 1440p, 4K — passed to the cart as its preferred resolution (`wc_host_info_t`); a cart that follows it (cartwheel games do) renders the largest frame of its own aspect that fits, and RetroArch shows it at that aspect; a cart that fits the aspect too (cartwheel's `fit` modes) renders the whole frame, no bars. The default is 1080p: a frontend package for a handheld should set it to the panel (Batocera and Knulli: `wasmcart.retroarchcore.wasmcart_resolution=640x480` in batocera.conf), since the core cannot see the screen. Applies when content loads or restarts (Quick Menu → Restart Content)
- **Runtime Code Generation**: Enabled (default) / Disabled, applied when content loads (`WASMCART_JIT=0` in the environment also turns it off). A cart that asks for it while it is off runs without it, and the core shows an on-screen notice on its first request saying so.
- **GPU API**: Auto (WebGPU when available, the default) / GL — which API a cart that imports both GL and WebGPU runs on (see "WebGPU carts"). With GL, a WebGPU-only cart does not start, and the core says so on screen. Applies when content loads or restarts.
- **Performance Log**: Disabled (default) / Enabled (`WASMCART_PERF_LOG=1` too) — once a second (and once more when the content closes, for the last part second), the frame rate and where each frame's time went, written to RetroArch's log and to `wasmcart-perf.log` in the save directory, with the time spent in the frontend's callbacks (`video_cb`, `audio_batch` with the samples pushed per frame against the cart's rate / 60, `input_poll`), where RetroArch can block. A `phases` line splits retro_run, in ms a frame: `input` (polling RetroArch and building the pads), `glstate` (the cart's GL state put back), `wc_render` (the cart's frame: its code, its imports and the WebGPU glue) with `gl_imports` its GL share, `host_frame` (the host's work around wc_render), `present` (the WebGPU frame into GL, the blit, video_cb), `audio`, and `wgpu_direct_calls` (WebGPU calls a frame that went straight to Dawn). A WebGPU cart gets one more line, `webgpu shared` (the cart's `wc_render`, the GPU copy into the shared texture, GL drawing it) or `webgpu readback` (the cart's `wc_render`, queueing the frame's copy, the time waiting for the frame shown, the GL upload, and the latency from queue to mapped: the GPU's frame and the copy). The cart's compiled code is kept between runs in `wasmcart/code` in the save directory (the next start runs the code V8 optimized last time from its first frame).

## Tests (Linux)

```bash
cmake -B build -DWASMCART_LIBRETRO_TESTS=ON && cmake --build build
test/run-tests.sh build <wasmcart-lua cart.wasc> <JIT engine cart.wasc>
```

The build must have WebGPU (configure with `-DWASMCART_WGPU_JS_DIR` and
`-DNATIVE_DAWN_DIR`, "WebGPU carts" above) and the machine a Vulkan GPU: the
WebGPU part runs wasmcart-native's fixtures on Dawn (dual cart on WebGPU and,
with GPU API GL, on GL; a WebGPU-only cart, restarted and refused; a surface
resize; a lost device; the performance log's WebGPU line), checking pixels.

`test/lr_frontend` is a headless libretro frontend (Mesa surfaceless EGL). The script restores a save state mid-run, carries a JIT cart's state across a restore and across sessions, checks the JIT-off notice, and carries a save (SAVE_RAM) across sessions in RetroArch's order (the .srm copied in after retro_load_game, before context_reset and the first retro_run), and restarts a cart (Restart Content: a changed Internal Resolution applies, the save carries over).

## Build Targets

| Platform | Status | Notes |
|----------|--------|-------|
| Linux x86_64 | Working | |
| Linux aarch64 | Working | |
| macOS x86_64 | Working | |
| macOS aarch64 | Working | |
| Windows x86_64 | Working | |
| Android aarch64 | Working | NDK r27c, API 33 |

## Pre-built binaries

Download from [Releases](https://github.com/wasmcart/wasmcart-libretro/releases) —
Linux (x86_64/aarch64), macOS (x86_64/aarch64), Windows x86_64 and Android aarch64.

The bundled ZIP reader is [miniz](https://github.com/richgel999/miniz) and manifest
parsing uses [cJSON](https://github.com/DaveGamble/cJSON); both are vendored under
`deps/`.

## License

MIT
