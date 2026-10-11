cmake_minimum_required(VERSION 3.16)
project(wasmcart-libretro C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# MSVC: force C++20, proper __cplusplus, and static CRT (match libnode)
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++20>" "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ─── Shared code from wasmcart-native (submodule) ────────────────────────

set(WC_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/deps/wasmcart-native")

# ─── Node.js / libnode ───────────────────────────────────────────────────

set(LIBNODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libnode" CACHE PATH "Path to libnode.a + headers")

if(NOT EXISTS "${LIBNODE_DIR}/libnode.a" AND NOT EXISTS "${LIBNODE_DIR}/libnode.lib")
    message(FATAL_ERROR "libnode not found at ${LIBNODE_DIR}\n"
        "Download from: https://github.com/wasmcart/build-libnode/releases\n"
        "  mkdir -p deps/libnode && curl -sL <url>/libnode-linux-x86_64.tar.gz | tar xz -C deps/libnode")
endif()

# ─── Source files ────────────────────────────────────────────────────────

add_library(wasmcart_libretro SHARED
    src/libretro.c
    src/glibc_compat.c
    ${WC_NATIVE}/src/cart_host.cpp
    ${WC_NATIVE}/src/gl_imports.cpp
    ${WC_NATIVE}/src/gl_procs.c
    ${WC_NATIVE}/src/gl_track.c
    ${WC_NATIVE}/src/wgpu_imports.cpp
    ${WC_NATIVE}/src/wgpu_gl_share.cpp
    ${WC_NATIVE}/src/code_cache.cpp
    ${WC_NATIVE}/src/asset_loader.c
    ${WC_NATIVE}/deps/miniz.c
    ${WC_NATIVE}/deps/cJSON.c
)

# No "lib" prefix — RetroArch expects "wasmcart_libretro.so" not "libwasmcart_libretro.so"
set_target_properties(wasmcart_libretro PROPERTIES PREFIX "")

target_include_directories(wasmcart_libretro PRIVATE
    src
    ${WC_NATIVE}/include
    ${WC_NATIVE}/src
    ${WC_NATIVE}/deps
    ${LIBNODE_DIR}/include
)

# GL: the core uses RetroArch's GL context. It needs the GLES3 *headers* (for the
# GL types + PFN typedefs) but links NO GL library — every GL entry point is
# loaded at runtime from the frontend's get_proc_address (see gl_procs.{h,c} +
# wc_host_set_gl_loader in src/libretro.c). Headers are vendored (Khronos, MIT) so
# the build is identical and self-contained on every platform: no system mesa-dev,
# no ANGLE, no EGL/GLESv2 link, no sidecar dylibs. Single-file core everywhere.
target_include_directories(wasmcart_libretro PRIVATE ${WC_NATIVE}/vendor/gl)

# libnode (+ platform system libs). NOTE: no GLESv2/EGL here — the GL layer loads
# GL from the frontend, so the core links no GL library on any platform.
if(WIN32)
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.lib winmm dbghelp ws2_32 crypt32 iphlpapi psapi userenv bcrypt)
elseif(ANDROID)
    # cpu-features: V8 calls android_getCpuFeatures
    add_library(cpufeatures STATIC ${ANDROID_NDK}/sources/android/cpufeatures/cpu-features.c)
    target_include_directories(cpufeatures PUBLIC ${ANDROID_NDK}/sources/android/cpufeatures)
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.a dl m log android cpufeatures)
elseif(APPLE)
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.a pthread dl m
        "-framework CoreFoundation" "-framework Security" "-framework SystemConfiguration")
else()
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.a pthread dl m)
    # Redirect sqrtf@GLIBC_2.43 (pulled in when linking on a 2.43 host) to the old
    # @GLIBC_2.2.5 version so the .so loads on older runtimes (e.g. the RetroDECK
    # flatpak's glibc). See src/glibc_compat.c.
    target_link_options(wasmcart_libretro PRIVATE -Wl,--wrap=sqrtf)
endif()

if(NOT MSVC)
    target_compile_options(wasmcart_libretro PRIVATE -Wno-unused-parameter)
endif()

# ─── WebGPU (wasmcart SPEC.md, "WebGPU") ─────────────────────────────────
# A cart that imports WebGPU runs on Dawn's own device (Vulkan on Linux),
# created by the core independent of the frontend's video driver; its frame
# is copied into RetroArch's GL framebuffer. Dawn (native-dawn's dawn.node and
# its library) and the reference host's JavaScript (wasmcart-native's bridge,
# wasmcart's src/wgpu) are loaded at runtime from a directory beside the core,
# wasmcart_libretro_wgpu/ ($WASMCART_WGPU_DIR overrides). Without it the core
# runs dual GL + WebGPU carts on GL and refuses WebGPU-only carts.
#   -DWASMCART_WGPU_JS_DIR=<wasmcart>/src/wgpu -DNATIVE_DAWN_DIR=<native-dawn>/<platform>
# (deps/wasmcart-native/scripts/fetch-wgpu-deps.sh fetches both) builds that
# directory beside the core.
target_compile_definitions(wasmcart_libretro PRIVATE WASMCART_WGPU_BESIDE_MODULE="wasmcart_libretro_wgpu")
set(WASMCART_WGPU_JS_DIR "" CACHE PATH "wasmcart's src/wgpu directory (host.js and the generated glue)")
set(NATIVE_DAWN_DIR "" CACHE PATH "native-dawn release archive for this platform, unpacked")
if(WASMCART_WGPU_JS_DIR AND NATIVE_DAWN_DIR)
    # the hot imports call Dawn's C API, and the frames are shared with GL
    # (wgpu_imports.cpp, wgpu_gl_share.cpp): Dawn's headers here (and on Linux
    # Vulkan's), its library resolved at run time from wasmcart_libretro_wgpu/
    target_include_directories(wasmcart_libretro PRIVATE "${NATIVE_DAWN_DIR}/include")
    target_compile_definitions(wasmcart_libretro PRIVATE WASMCART_WGPU_NATIVE)
    set(WGPU_OUT "$<TARGET_FILE_DIR:wasmcart_libretro>/wasmcart_libretro_wgpu")
    file(WRITE "${CMAKE_BINARY_DIR}/wgpu-package.json" "{ \"type\": \"module\" }\n")
    if(WIN32)
        set(WGPU_DAWN_FILES "${NATIVE_DAWN_DIR}/bin/dawn.node" "${NATIVE_DAWN_DIR}/bin/webgpu_dawn.dll"
            "${NATIVE_DAWN_DIR}/bin/dxcompiler.dll" "${NATIVE_DAWN_DIR}/bin/dxil.dll")
    elseif(APPLE)
        set(WGPU_DAWN_FILES "${NATIVE_DAWN_DIR}/lib/dawn.node" "${NATIVE_DAWN_DIR}/lib/libwebgpu_dawn.dylib")
    else()
        set(WGPU_DAWN_FILES "${NATIVE_DAWN_DIR}/lib/dawn.node" "${NATIVE_DAWN_DIR}/lib/libwebgpu_dawn.so")
    endif()
    add_custom_target(wasmcart-libretro-wgpu ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WGPU_OUT}/src/wgpu"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WC_NATIVE}/src/wgpu_bridge.cjs" ${WGPU_DAWN_FILES} "${WGPU_OUT}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WASMCART_WGPU_JS_DIR}/host.js" "${WASMCART_WGPU_JS_DIR}/release.js"
            "${WASMCART_WGPU_JS_DIR}/glue-loader.js" "${WASMCART_WGPU_JS_DIR}/emdawnwebgpu-glue.mjs"
            "${WASMCART_WGPU_JS_DIR}/emdawnwebgpu-glue-manifest.js" "${WGPU_OUT}/src/wgpu/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WASMCART_WGPU_JS_DIR}/../cartMemory.js" "${WGPU_OUT}/src/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/wgpu-package.json" "${WGPU_OUT}/package.json"
        COMMENT "WebGPU support -> wasmcart_libretro_wgpu/")
    add_dependencies(wasmcart-libretro-wgpu wasmcart_libretro)
endif()

# RPATH (Linux/macOS — find libnode.so next to the .so/.dylib)
if(NOT WIN32)
    set_target_properties(wasmcart_libretro PROPERTIES
        BUILD_RPATH "${LIBNODE_DIR}"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# ─── Regression tests (Linux): test/run-tests.sh ─────────────────────────
# lr_frontend is a minimal headless libretro frontend (Mesa surfaceless EGL).

option(WASMCART_LIBRETRO_TESTS "Build the test frontend (needs EGL and GLESv2)" OFF)
if(WASMCART_LIBRETRO_TESTS AND UNIX AND NOT APPLE AND NOT ANDROID)
    add_executable(lr_frontend test/lr_frontend.c)
    target_include_directories(lr_frontend PRIVATE src ${WC_NATIVE}/vendor/gl)
    target_link_libraries(lr_frontend PRIVATE EGL GLESv2 dl)
endif()

# ─── Status ──────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "wasmcart-libretro:")
message(STATUS "  wasmcart-native: ${WC_NATIVE}")
message(STATUS "  libnode: ${LIBNODE_DIR}")
message(STATUS "  GL:      from RetroArch get_proc_address (no GL library linked)")
message(STATUS "")
