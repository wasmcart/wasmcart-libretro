#!/bin/sh
# Install wasmcart as a system on a Batocera box. Run ON the box as root, from
# this directory, with wasmcart_libretro.so (linux build for the box's arch)
# sitting next to this script. Everything lands in /userdata, which survives
# reboots and upgrades; the read-only /usr tree only ever gets symlinks, put
# back on every boot by the 'wasmcart' user service.
set -e
D=/userdata/system/wasmcart
mkdir -p "$D/theme" /userdata/roms/wasmcart /userdata/system/services /userdata/system/configs/emulationstation
cp wasmcart_libretro.so "$D/"
cp ../../wasmcart_libretro.info "$D/" 2>/dev/null || cp wasmcart_libretro.info "$D/"
cp ../es-de/logos/wasmcart.svg "$D/theme/wasmcart.svg" 2>/dev/null || cp wasmcart.svg "$D/theme/wasmcart.svg"
cp es_systems_wasmcart.cfg /userdata/system/configs/emulationstation/
cp wasmcart.service.sh /userdata/system/services/wasmcart
chmod +x /userdata/system/services/wasmcart
grep -q '^wasmcart.emulator=' /userdata/system/batocera.conf || \
  printf '\n## wasmcart (custom system)\nwasmcart.emulator=libretro\nwasmcart.core=wasmcart\n' >> /userdata/system/batocera.conf
batocera-services enable wasmcart
/userdata/system/services/wasmcart start
echo "Installed. Drop .wasc carts in /userdata/roms/wasmcart, then restart EmulationStation"
echo "(Menu > Quit > Restart, or: batocera-es-swissknife --restart)."
