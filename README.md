# wasmcart-libretro

RetroArch libretro core for wasmcart. Runs `.wasc` game carts in RetroArch with full gamepad, audio, save state, shader, and netplay support. Powered by V8 via libnode for instant WASM startup.

## Working Carts

| Cart | Type | Engine |
|------|------|--------|
| Snake | 2D framebuffer | C/Emscripten |
| Warlords | GL (Godot 4.4) | Godot GLES3 Compatibility |
| RoboBlast TPS | GL (Godot 4.4) | Godot GLES3, 3D |
| OpenArena | GL (ioquake3) | renderergl2 GLES |

## Build

```bash
# 1. Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v24.14.1/libnode-linux-x86_64.tar.gz | tar xz -C deps/libnode

# 2. Build the core
mkdir build && cd build
cmake ..
make -j$(nproc)

# 3. Install
CORES=~/.var/app/org.libretro.RetroArch/config/retroarch/cores
cp wasmcart_libretro.so $CORES/
cp ../wasmcart_libretro.info $CORES/
```

Pre-built libnode downloads for all platforms are available at [wasmcart/build-libnode releases](https://github.com/wasmcart/build-libnode/releases). Replace `linux-x86_64` with your platform: `linux-aarch64`, `macos-x86_64`, `macos-aarch64`, `windows-x86_64`, or `android-aarch64`.

## Usage

```bash
# Flatpak RetroArch
CORES=~/.var/app/org.libretro.RetroArch/config/retroarch/cores
flatpak run \
  --env=LD_LIBRARY_PATH=$CORES \
  --filesystem=/path/to/your/roms \
  org.libretro.RetroArch \
  -L $CORES/wasmcart_libretro.so /path/to/game.wasc

# Native RetroArch
retroarch -L /path/to/wasmcart_libretro.so /path/to/game.wasc
```

## Architecture

```
wasmcart_libretro.so
├── libretro.c           <- RetroArch <-> wasmcart bridge
├── cart_host.cpp         <- V8 WASM compile/instantiate/call
├── gl_imports.cpp        <- 160 GL functions as V8 FunctionCallbacks
├── asset_loader.c        <- .wasc ZIP reading
├── miniz.c / cJSON.c     <- ZIP + JSON deps
└── libnode.a             <- V8 runtime (linked)
```

The only libretro-specific file is `libretro.c`. The remaining source files are shared with [wasmcart-native](https://github.com/nicedoc/wasmcart-native).

## Key Implementation Details

### V8 / libnode
- V8's Liftoff baseline compiler starts WASM instantly -- 52MB Godot loads in ~356ms
- No compilation cache needed (unlike wasmtime's 29s cold start)
- `--experimental-wasm-exnref` V8 flag enabled for standard WASM exceptions
- Auto-stub missing imports (V8 doesn't have wasmtime's default import mechanism)

### GL Carts
- Deferred init: GL carts need RetroArch's GL context before `_initialize`/`wc_init`
- `retro_load_game` compiles + instantiates WASM but defers cart init
- `on_context_reset` calls `wc_host_finish_init()` after GL is available
- GL detection via import pre-scan (checks for "gl" module or `env.glXxx` functions)
- GL context: tries OpenGL Core 3.3, falls back to GLES3, then GLES2
- FBO redirect: cart renders to capture FBO, `wc_gl_blit_to_fbo()` blits to RetroArch's per-frame FBO
- Preferred resolution: 1920x1080 passed to cart via host_info (cart decides actual render resolution, RetroArch handles display scaling)

### 2D Carts
- Framebuffer pixels passed directly to `video_cb()` -- ARGB8888 = XRGB8888
- Init runs on first `retro_run()` call (no GL context needed)

### Audio
- F32 -> S16 conversion (libretro only accepts int16 audio)
- Ring buffer drain same as native host

### Input
- Full 4-player gamepad: buttons, analog sticks, triggers
- Libretro joypad IDs mapped to wasmcart button bitmask

## RetroArch Features (free)

- Shaders (CRT, scanlines, upscaling)
- Controller remapping
- Recording / streaming
- Netplay (WASM is deterministic)
- Save states (for small carts)
- RetroAchievements (via WASM memory inspection)

## File Structure

```
wasmcart-libretro/
├── src/
│   ├── libretro.c              <- RetroArch <-> wasmcart bridge
│   ├── libretro.h              <- Standard libretro header
│   ├── cart_host.cpp            <- V8 WASM engine
│   ├── cart_host.h
│   ├── gl_imports.cpp           <- GL function bridge
│   ├── asset_loader.c           <- .wasc ZIP reader
│   ├── abi.h
│   └── egl_context.h
├── include/
│   └── wasmcart_host.h          <- Public API header
├── deps/
│   ├── miniz.c / miniz.h        <- ZIP library
│   ├── cJSON.c / cJSON.h        <- JSON parser
│   └── libnode/                  <- Pre-built (download separately)
├── CMakeLists.txt
├── wasmcart_libretro.info
└── README.md
```
