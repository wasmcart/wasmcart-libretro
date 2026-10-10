#!/usr/bin/env bash
# Regression tests for the core, run through test/lr_frontend (a headless
# libretro frontend). Build with -DWASMCART_LIBRETRO_TESTS=ON first.
#
#   test/run-tests.sh <build dir> <wasmcart-lua cart.wasc> <JIT engine cart.wasc>
#
# 1. A wasmcart-lua cart is saved, runs on, and is restored mid-run: the
#    restore moves the audio write cursor backwards (segfaulted before
#    wasmcart-native 30faf2b).
# 2. A JIT engine cart across a restore in the same session, and across
#    sessions (state saved, core started again, state loaded): the core must
#    unlink every slot and change wc_jit_config key 7, or the engine calls a
#    slot that is gone and traps.
# 3. Runtime code generation switched off by the core option: the cart's first
#    JIT call puts the notice on screen (SET_MESSAGE_EXT).
set -u
BUILD=${1:?build dir}; LUA=${2:?wasmcart-lua cart}; JIT=${3:?JIT engine cart}
FE="$BUILD/lr_frontend"; CORE="$BUILD/wasmcart_libretro.so"
# Copies, so the core's wasmcart.log lands here and not next to the carts.
WORK=$(mktemp -d "$BUILD/lrtest.XXXXXX"); trap 'rm -rf "$WORK"' EXIT
cp "$LUA" "$WORK/lua.wasc"; cp "$JIT" "$WORK/jit.wasc"
pass=0; fail=0
run() {
  local name=$1; shift
  if timeout -s KILL 180 "$FE" "$CORE" "$@" > "$WORK/out.txt" 2>&1; then
    echo "ok   $name"; pass=$((pass + 1))
  else
    echo "FAIL $name (exit $?)"; grep -E "lr_frontend|trapped" "$WORK/out.txt" | tail -3 | sed 's/^/     /'; fail=$((fail + 1))
  fi
}
run "lua cart: restore mid-run (audio cursor)" "$WORK/lua.wasc" --frames 300 --save-at 60 --restore-at 200
run "JIT cart: restore in the same session"   "$WORK/jit.wasc" --frames 300 --save-at 60 --restore-at 200
run "JIT cart: save a state"                  "$WORK/jit.wasc" --frames 160 --save-at 150 --state-out "$WORK/jit.state"
run "JIT cart: load it in a new session"      "$WORK/jit.wasc" --frames 300 --restore-at 100 --state-in "$WORK/jit.state"
run "JIT off: notice on screen"               "$WORK/jit.wasc" --frames 60 --option wasmcart_jit=disabled \
    --expect-message "Runtime code generation is OFF"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
