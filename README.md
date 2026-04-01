# wasmcart-libretro

RetroArch libretro core for wasmcart. Runs `.wasc` game carts in RetroArch with full gamepad, audio, save state, shader, and netplay support. Powered by V8 via libnode for instant WASM startup.

## Working Carts

| Cart | Type | Engine |
|------|------|--------|
| Snake | 2D framebuffer | C/Emscripten |
| Three.js Demo | GL (WebGL2) | Three.js direct |
| Warlords | GL (Godot 4.4) | Godot GLES3 Compatibility |
| RoboBlast TPS | GL (Godot 4.4) | Godot GLES3, 3D |
| OpenArena | GL (ioquake3) | renderergl2 GLES |

## Build

```bash
# 1. Clone with submodule
git clone --recursive https://github.com/wasmcart/wasmcart-libretro.git
cd wasmcart-libretro

# 2. Download pre-built libnode
mkdir -p deps/libnode
curl -sL https://github.com/wasmcart/build-libnode/releases/download/v24.14.1/libnode-linux-x86_64.tar.gz | tar xz -C deps/libnode

# 3. Build the core
mkdir build && cd build
cmake ..
make -j$(nproc)

# 4. Install
CORES=~/.var/app/org.libretro.RetroArch/config/retroarch/cores
cp wasmcart_libretro.so $CORES/
cp ../wasmcart_libretro.info $CORES/
```

Pre-built libnode downloads for all platforms at [wasmcart/build-libnode releases](https://github.com/wasmcart/build-libnode/releases).

macOS and Windows also need ANGLE for GLES3 headers + libs. Set `-DANGLE_DIR=path/to/angle` in cmake.

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

The wasmcart GL surface is **WebGL2 / OpenGL ES 3.0** — the same on all hosts. The host reports `GL_VERSION = "OpenGL ES 3.0 wasmcart"` regardless of the actual driver. Real driver extensions pass through for carts that need them (e.g., Godot texture format detection).

RetroArch provides a Core 3.3 GL context (via GLX). ES 3.0 shaders (`#version 300 es`) work on Core 3.3 via `GL_ARB_ES3_compatibility`. The FBO redirect intercepts `glBindFramebuffer(0)` → capture FBO with depth+stencil, then blits to RetroArch's hw_render FBO after each frame.

## Core Options

- **Internal Resolution**: 640x480, 720p, 1080p, 1440p, 4K — passed to cart as preferred resolution

## Build Targets

| Platform | Status | Notes |
|----------|--------|-------|
| Linux x86_64 | Working | |
| Linux aarch64 | Working | |
| macOS x86_64 | Working | Requires ANGLE |
| macOS aarch64 | Working | Requires ANGLE |
| Windows x86_64 | Working | Requires ANGLE |
| Android aarch64 | Working | NDK r27c, API 33 |

## Pre-built binaries

Download from [Releases](https://github.com/wasmcart/wasmcart-libretro/releases).

## License

MIT
