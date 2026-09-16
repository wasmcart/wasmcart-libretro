#!/bin/sh
# Batocera user service: expose the wasmcart libretro core (kept in /userdata)
# to configgen/RetroArch, which only look in the read-only /usr tree.
# Enabled via: batocera-services enable wasmcart   (writes system.services in batocera.conf)
D=/userdata/system/wasmcart
THEME=/usr/share/emulationstation/themes/es-theme-carbon
case "$1" in
  start)
    ln -sf "$D/wasmcart_libretro.so"   /usr/lib/libretro/wasmcart_libretro.so
    ln -sf "$D/wasmcart_libretro.info" /usr/share/libretro/info/wasmcart_libretro.info
    ln -sf "$D/theme/wasmcart.svg"     "$THEME/art/logos/wasmcart.svg"
    ln -sf "$D/theme/wasmcart.svg"     "$THEME/art/logos/wasmcart-w.svg"
    [ -f "$D/theme/wasmcart.png" ] && ln -sf "$D/theme/wasmcart.png" "$THEME/art/consoles/wasmcart.png"
    [ -f "$D/theme/wasmcart-bg.jpg" ] && ln -sf "$D/theme/wasmcart-bg.jpg" "$THEME/art/background/wasmcart.jpg"
    ;;
  stop)
    rm -f /usr/lib/libretro/wasmcart_libretro.so /usr/share/libretro/info/wasmcart_libretro.info \
          "$THEME/art/logos/wasmcart.svg" "$THEME/art/logos/wasmcart-w.svg" \
          "$THEME/art/consoles/wasmcart.png" "$THEME/art/background/wasmcart.jpg"
    ;;
esac
exit 0
