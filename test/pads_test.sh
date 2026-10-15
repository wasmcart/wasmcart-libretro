#!/bin/sh
# Four pads: each RetroPad port reaches the cart in its own wc_pad_t
# (wasmcart-native's padcart fixture shows every pad). libretro does not say
# whether a port has a controller, so port 0 is always connected and ports
# 1-3 connect on their first input, each in its own slot; a port the frontend
# sets to None, or one past its number of users, is not connected. Through
# 0.10.0 ports 1-3 were never connected, so a cart never saw players 2-4.
#
#   test/pads_test.sh <build dir>     (built with -DWASMCART_LIBRETRO_TESTS=ON)
BUILD=$(cd "${1:?build dir}" && pwd)
FE="$BUILD/lr_frontend"; CORE="$BUILD/wasmcart_libretro.so"
WORK=$(mktemp -d "$BUILD/padtest.XXXXXX"); trap 'rm -rf "$WORK"' EXIT
cp "$(dirname "$0")/../deps/wasmcart-native/test/padcart.wasc" "$WORK/pad.wasc"
fail=0
run() {
  local name=$1; shift
  if timeout -s KILL 120 "$FE" "$CORE" "$@" > "$WORK/out.txt" 2>&1; then echo "ok   $name"
  else echo "FAIL $name (exit $?)"; grep -E "lr_frontend|trapped" "$WORK/out.txt" | tail -3 | sed 's/^/     /'; fail=1; fi
}
cat > "$WORK/pads.txt" <<'PADS'
5 b
p1 5 a 0.5 0
p2 5 y
p3 5 x
p2 12 -
PADS
# pixel X Y of a --shot PPM, as RRGGBB
ppm_px() { od -An -tx1 -j $(( $(head -n3 "$1" | wc -c) + ($3 * 64 + $2) * 3 )) -N3 "$1" | tr -d ' \n'; }
check_pads() { # name, shot, then X,Y,RRGGBB...
  name=$1 shot=$2 bad=""; shift 2
  if [ ! -s "$shot" ]; then echo "FAIL $name: no frame"; fail=1; return; fi
  for c in "$@"; do
    got=$(ppm_px "$shot" "${c%%,*}" "$(echo "$c" | cut -d, -f2)")
    [ "$got" = "${c##*,}" ] || bad="$bad ${c%,*} is $got (want ${c##*,})"
  done
  if [ -z "$bad" ]; then echo "ok   $name"; else echo "FAIL $name:$bad"; fail=1; fi
}
# padcart's columns: pad i at x = 16i..16i+15; row 8 connected (22ff22, else ff2222), row 24 the
# buttons mask as RRGGBB (south 1, east 2, west 4, north 8), row 40 the left stick ((v >> 8) + 128)
run "pads: four RetroPads drive the pad cart" "$WORK/pad.wasc" --frames 16 --input "$WORK/pads.txt" \
    --shot 3:"$WORK/pad3.ppm" --shot 10:"$WORK/pad10.ppm" --shot 15:"$WORK/pad15.ppm" --unplug-port 14:3
check_pads "pads: before any input only pad 0 is connected" "$WORK/pad3.ppm" 8,8,22ff22 24,8,ff2222 40,8,ff2222 56,8,ff2222
check_pads "pads: ports 1-3 connect on their first input, each in its own slot" "$WORK/pad10.ppm" \
    8,8,22ff22 24,8,22ff22 40,8,22ff22 56,8,22ff22 8,24,000001 24,24,000002 40,24,000004 56,24,000008 24,40,bf8000
check_pads "pads: a released port stays connected; a port set to None is not" "$WORK/pad15.ppm" \
    40,8,22ff22 40,24,000000 56,8,ff2222 56,24,000000
run "pads: the frontend's user count limits the pads" "$WORK/pad.wasc" --frames 10 --input "$WORK/pads.txt" \
    --max-users 2 --shot 9:"$WORK/pad9.ppm"
check_pads "pads: ports past the user count are not connected" "$WORK/pad9.ppm" 24,8,22ff22 40,8,ff2222 56,8,ff2222
exit $fail
