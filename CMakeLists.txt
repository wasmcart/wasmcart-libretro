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
    ${WC_NATIVE}/src/cart_host.cpp
    ${WC_NATIVE}/src/gl_imports.cpp
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

# GLES3/EGL — system on Linux/Android, ANGLE on macOS/Windows
set(ANGLE_DIR "" CACHE PATH "Path to ANGLE (lib/ + include/)")
if(ANGLE_DIR AND EXISTS "${ANGLE_DIR}/include")
    target_include_directories(wasmcart_libretro PRIVATE ${ANGLE_DIR}/include)
    if(APPLE)
        target_link_libraries(wasmcart_libretro PRIVATE
            ${ANGLE_DIR}/lib/libEGL.dylib
            ${ANGLE_DIR}/lib/libGLESv2.dylib)
    elseif(WIN32)
        target_link_libraries(wasmcart_libretro PRIVATE
            ${ANGLE_DIR}/lib/libEGL.lib
            ${ANGLE_DIR}/lib/libGLESv2.lib)
    endif()
endif()

# libnode
if(WIN32)
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.lib winmm dbghelp ws2_32 crypt32 iphlpapi psapi userenv bcrypt)
elseif(ANDROID)
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.a dl m log GLESv3 EGL android)
elseif(APPLE)
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.a pthread dl m
        "-framework CoreFoundation" "-framework Security" "-framework SystemConfiguration")
else()
    target_link_libraries(wasmcart_libretro PRIVATE ${LIBNODE_DIR}/libnode.a pthread dl m)
endif()

# GL (provided by RetroArch's context, but we need headers + link for FBO ops)
if(NOT ANDROID)
    find_library(GLESV2_LIB GLESv2)
    find_library(EGL_LIB EGL)
    if(GLESV2_LIB)
        target_link_libraries(wasmcart_libretro PRIVATE ${GLESV2_LIB})
    endif()
    if(EGL_LIB)
        target_link_libraries(wasmcart_libretro PRIVATE ${EGL_LIB})
    endif()
else()
    target_link_libraries(wasmcart_libretro PRIVATE GLESv2 EGL log)
endif()

if(NOT MSVC)
    target_compile_options(wasmcart_libretro PRIVATE -Wno-unused-parameter)
endif()

# RPATH (Linux/macOS — find libnode.so next to the .so/.dylib)
if(NOT WIN32)
    set_target_properties(wasmcart_libretro PROPERTIES
        BUILD_RPATH "${LIBNODE_DIR}"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# ─── Status ──────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "wasmcart-libretro:")
message(STATUS "  wasmcart-native: ${WC_NATIVE}")
message(STATUS "  libnode: ${LIBNODE_DIR}")
message(STATUS "  GLESv2:  ${GLESV2_LIB}")
message(STATUS "  EGL:     ${EGL_LIB}")
message(STATUS "")
