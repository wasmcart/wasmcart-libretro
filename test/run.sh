#!/usr/bin/env bash
# test/run.sh -- run the built core against small carts in a headless frontend.
#
#   test/run.sh [core.so]        (default: build/wasmcart_libretro.so)
#
# Needs: wasi-sdk (WASI_SDK, or ../../mine/wasi-sdk style default below), a C
# compiler, and Mesa's surfaceless EGL (libEGL + libGLESv2). Builds into
# build/test/, which build/ already keeps out of git.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
core="${1:-$root/build/wasmcart_libretro.so}"
out="$root/build/test"
mkdir -p "$out"

clang="${WASI_SDK:-$HOME/code/mine/wasi-sdk}/bin/clang"
[ -x "$clang" ] || { echo "no wasi-sdk clang at $clang (set WASI_SDK)"; exit 2; }
[ -f "$core" ] || { echo "no core at $core (build it first)"; exit 2; }

cc -O1 -Wall -o "$out/core_test" "$here/core_test.c" -I"$root/src" -lEGL -lGLESv2 -ldl || exit 2

fail=0
pass=0
# cart MODE: build test/allocart.c in that allocator shape
cart() {
    "$clang" --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--allow-undefined \
        "-DMODE_$1" -o "$out/$1.wasm" "$here/allocart.c" || exit 2
}
# run MODE: run it headless; leaves the log in $log and the RESULT line in $res
run() {
    log="$out/$1.log"
    SDL_AUDIODRIVER=dummy timeout -s KILL 60 "$out/core_test" "$core" "$out/$1.wasm" 6 > "$log" 2>&1
    res="$(grep '^RESULT' "$log")"
}
expect() { # what, condition (0 = held)
    if [ "$2" -eq 0 ]; then echo "  ok    $1"; pass=$((pass + 1));
    else echo "*** FAIL $1 (log: $log)"; fail=$((fail + 1)); fi
}
has() { grep -qF -- "$1" "$log"; echo $?; }
lacks() { grep -qF -- "$1" "$log" && echo 1 || echo 0; }
count() { grep -cF -- "$1" "$log"; }
is() { [ "$1" = "$2" ]; echo $?; }

NOALLOC="does not export wc_alloc/wc_free"
MISSING="glGetString must write into the cart's memory, but the cart does not export wc_alloc/wc_free (its malloc, if any, is not used). Export wc_alloc(size, align) and wc_free(ptr): wasmcart.h defines them for C/C++ (include it), wasmcart's CMake helper exports them, and Rust carts get them from the wasmcart-alloc crate. See SPEC.md, \"Cart memory the host writes\"."

for m in WCALLOC QUIET NONE_HIT MALLOC MALLOCFREE MISALIGN BAD_OOB BAD_ZERO; do cart $m; done

echo "wc_alloc cart: GL strings and buffer mappings land in the cart's own allocations"
run WCALLOC
expect "runs every frame"            $(has "frames=6 ")
expect "strings and both mappings check out" $(has "ok=1 ")
expect "2 strings (cached) + 2 mappings = 4 allocations" $(has "allocs=4 ")
expect "strings kept, each mapping freed at unmap" $(has "frees=2 ")
expect "no deprecation warning"      $(lacks "deprecat")
expect "not trapped"                 $(has "trapped=0")

echo "no allocator, never reaches a path: loads and runs"
run QUIET
expect "runs every frame"            $(has "frames=6 ")
expect "ok"                          $(has "ok=1 ")
expect "no allocator error"          $(lacks "$NOALLOC")
expect "no deprecation warning"      $(lacks "deprecat")
expect "not trapped"                 $(has "trapped=0")

echo "no allocator, asks for a GL string: a clear error stops the cart"
run NONE_HIT
expect "error names glGetString and the fix" $(has "$MISSING")
expect "cart stopped"                $(has "trapped=1")
expect "stopped in the frame that asked" $(has "frames=2 ")
expect "string never handed back"    $(has "ok=0 ")

# wasmcart 0.32: a cart's malloc is never used. Without wc_alloc/wc_free it
# gets exactly the no-allocator error, with or without free/memalign.
for m in MALLOC MALLOCFREE; do
    echo "$m cart (no wc_alloc): the same error as no allocator"
    run $m
    expect "error names glGetString and the fix" $(has "$MISSING")
    expect "cart stopped"                $(has "trapped=1")
    expect "stopped in the frame that asked" $(has "frames=2 ")
    expect "string never handed back"    $(has "ok=0 ")
    expect "its malloc never called"     $(has "allocs=0 ")
    expect "no deprecation warning"      $(lacks "deprecat")
done

echo "wc_alloc ignores alignment: the mapping is rejected"
run MISALIGN
expect "strings (align 1) still worked" $(has "ok=1 ")
expect "error names the alignment"   $(has "glMapBufferRange: the cart's wc_alloc(64, 16) returned 0x")
expect "error says why"              $(has "which is not 16-byte aligned")
expect "cart stopped"                $(has "trapped=1")
expect "stopped in the frame that mapped" $(has "frames=3 ")

echo "wc_alloc answers outside memory: rejected"
run BAD_OOB
expect "error names the pointer"     $(has "glGetString: the cart's wc_alloc(")
expect "error says why"              $(has "returned 0xffffff00, which is outside its")
expect "cart stopped"                $(has "trapped=1")
expect "string never handed back"    $(has "ok=0 ")

echo "wc_alloc answers 0: out of memory"
run BAD_ZERO
expect "error says out of memory"    $(has "glGetString: the cart's wc_alloc(")
expect "... (out of memory)"         $(has "returned 0 (out of memory)")
expect "cart stopped"                $(has "trapped=1")

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
