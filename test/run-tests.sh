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
# 3. Input actions: the port's device type reaches the cart. The action cart
#    (wasmcart-native's test fixture) logs its prompt on every input revision:
#    once at start (port 0 is a RetroPad, a gamepad), once more when port 0 is
#    unplugged (device unknown).
# 4. Runtime code generation switched off by the core option: the cart's first
#    JIT call puts the notice on screen (SET_MESSAGE_EXT).
# 5. A GLES2-only frontend (an old handheld such as a PowerVR SGX544; the
#    frontend refuses GLES3 and GL core and grants GLES2, with a real ES 2.0
#    context): a 2D cart (snake, wasmcart-native's fixture) presents through
#    the software video path; a GL cart is refused with an on-screen message
#    and the screen stays black. A driver that hands back an ES3 context for
#    the ES2 request runs the GL cart. On GLES3 the 2D cart still presents
#    through GL.
# 6. Saves persist across sessions, in RetroArch's SRAM order: SAVE_RAM's
#    size and data are read and the .srm copied in right after
#    retro_load_game, before context_reset and the first retro_run (the
#    save region was size 0 then, so nothing loaded and every start was a
#    fresh game). The save cart (wasmcart-native's fixture) counts its runs
#    in the save region and shows the count wc_init read: red 0, green 1.
# 7. Restart Content (retro_reset) starts a fresh instance of the cart with
#    the current core options and the current SAVE_RAM: the rescart fixture
#    (test/rescart.c, built here with wasi-sdk) renders at the preferred size
#    and counts its runs in its save region. A smaller Internal Resolution
#    applies on reset (SET_GEOMETRY), a larger one too (SET_SYSTEM_AV_INFO),
#    the old instance's GL objects are released, and the restarted cart reads
#    the save the first instance wrote (green after a fresh start: its second
#    run; blue after an .srm of one run: its third). The 2D save cart
#    restarts the same way.
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
ACTION="$(dirname "$0")/../deps/wasmcart-native/test/actioncart.wasc"
cp "$ACTION" "$WORK/action.wasc"
run "input actions: the action cart runs"     "$WORK/action.wasc" --frames 30 --unplug-at 15
if [ "$(grep -c "Press \[A\] to Pedal, \[Left Stick\] to Steer" "$WORK/out.txt")" -eq 2 ]; then
  echo "ok   input actions: the device change reaches the cart"; pass=$((pass + 1))
else
  echo "FAIL input actions: the device change reaches the cart"; grep "Press" "$WORK/out.txt" | sed 's/^/     /'; fail=$((fail + 1))
fi
SNAKE="$(dirname "$0")/../deps/wasmcart-native/test/snake.wasc"
cp "$SNAKE" "$WORK/snake.wasc"
# Snake's food at 162,194 is ff2222 (red) in its ARGB8888 framebuffer:
# the GL upload once showed it blue (2222ff).
run "GLES3: 2D cart presents through GL"      "$WORK/snake.wasc" --frames 60 --expect-video hw --expect-pixel 162,194,ff2222
run "GLES2 only: 2D cart presents in software" "$WORK/snake.wasc" --frames 60 --gles2 real --expect-video sw \
    --expect-pixel 162,194,ff2222
run "GLES2 only: GL cart refused on screen"   "$WORK/lua.wasc" --frames 60 --gles2 real --expect-video black \
    --expect-message "GLES 3.0 required"
run "ES2 request, ES3 context: GL cart runs"  "$WORK/lua.wasc" --frames 60 --gles2 es3 --expect-video hw
SAVE="$(dirname "$0")/../deps/wasmcart-native/test/savecart.wasc"
cp "$SAVE" "$WORK/save.wasc"
run "save: a first run starts from no save"   "$WORK/save.wasc" --frames 10 --sram-out "$WORK/save.srm" \
    --expect-pixel 10,10,ff2222
check_srm() { # name, expected first byte
  if [ "$(od -An -tx1 -N1 "$WORK/save.srm" 2>/dev/null | tr -d ' ')" = "$2" ]; then
    echo "ok   $1"; pass=$((pass + 1))
  else
    echo "FAIL $1"; od -An -tx1 "$WORK/save.srm" 2>&1 | sed 's/^/     /'; fail=$((fail + 1))
  fi
}
check_srm "save: the .srm holds the cart's save" 01
run "save: the next run starts from it"       "$WORK/save.wasc" --frames 10 --sram-in "$WORK/save.srm" \
    --sram-out "$WORK/save.srm" --expect-pixel 10,10,22ff22
check_srm "save: and saves on top of it" 02

clang="${WASI_SDK:-$HOME/code/mine/wasi-sdk}/bin/clang"
if [ ! -x "$clang" ]; then echo "FAIL reset: no wasi-sdk clang at $clang (set WASI_SDK)"; fail=$((fail + 1)); else
"$clang" --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--allow-undefined -o "$WORK/res.wasm" "$(dirname "$0")/rescart.c"
run "reset: a lower Internal Resolution applies" "$WORK/res.wasm" --frames 20 --option wasmcart_resolution=960x540 \
    --set-option 10:wasmcart_resolution=480x270 --reset-at 10 --expect-size 480x270 --expect-pixel 5,5,22ff22
grep -q "SET_GEOMETRY 480x270" "$WORK/out.txt" && grep -q "released 1 GL objects" "$WORK/out.txt" \
  && { echo "ok   reset: geometry announced, the old instance's GL objects released"; pass=$((pass + 1)); } \
  || { echo "FAIL reset: geometry announced, the old instance's GL objects released"; grep -E "GEOMETRY|released" "$WORK/out.txt" | sed 's/^/     /'; fail=$((fail + 1)); }
run "reset: a higher Internal Resolution applies" "$WORK/res.wasm" --frames 20 --option wasmcart_resolution=480x270 \
    --set-option 10:wasmcart_resolution=1920x1080 --reset-at 10 --expect-size 1920x1080 --expect-pixel 1900,1060,22ff22
grep -q "SET_SYSTEM_AV_INFO 1920x1080" "$WORK/out.txt" \
  && { echo "ok   reset: a larger frame reinitializes video"; pass=$((pass + 1)); } \
  || { echo "FAIL reset: a larger frame reinitializes video"; grep -E "GEOMETRY|AV_INFO" "$WORK/out.txt" | sed 's/^/     /'; fail=$((fail + 1)); }
printf '\x01\x00\x00\x00' > "$WORK/save.srm"
run "reset: the restarted cart reads the current save" "$WORK/res.wasm" --frames 20 --sram-in "$WORK/save.srm" \
    --reset-at 10 --sram-out "$WORK/save.srm" --expect-pixel 5,5,2222ff
check_srm "reset: and saves on top of it" 03
printf '\x01\x00\x00\x00' > "$WORK/save.srm"
run "reset: a 2D cart restarts with its save" "$WORK/save.wasc" --frames 20 --sram-in "$WORK/save.srm" \
    --reset-at 10 --sram-out "$WORK/save.srm" --expect-pixel 10,10,2222ff
check_srm "reset: the 2D cart saves on top of it" 03
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
