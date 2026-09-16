# Batocera integration

Adds a **"wasmcart"** system to Batocera (tested: Batocera 43.1, Raspberry Pi 5,
RetroArch 1.22.2, Mesa V3D on Wayland/labwc) without rebuilding the image.

## Why it needs a service

Batocera's launcher (`configgen`) only looks for libretro cores in
`/usr/lib/libretro` and their `.info` files in `/usr/share/libretro/info`, and
the system's EmulationStation theme only looks for logos inside
`/usr/share/emulationstation/themes/<theme>/art/logos/`. All of `/usr` is a RAM
overlay that is wiped on reboot (and `batocera-save-overlay` is lost on upgrade),
so the real files live in `/userdata/system/wasmcart/` and a user service
(`/userdata/system/services/wasmcart`, run on every boot) drops symlinks into
the overlay. `custom.sh` is deprecated in Batocera 43; services are the
replacement.

## Install

```sh
# on the box (ssh root@<ip>, password linux)
mkdir -p /tmp/wc && cd /tmp/wc
# copy in: wasmcart_libretro.so (linux-<arch> release), wasmcart_libretro.info,
#          es_systems_wasmcart.cfg, wasmcart.service.sh, install.sh, wasmcart.svg
sh install.sh
```

Then put `.wasc` files in `/userdata/roms/wasmcart/` and restart EmulationStation
(es_systems is only read at startup).

What `install.sh` does, by hand:

| What | Where |
| --- | --- |
| core + `.info` | `/userdata/system/wasmcart/` |
| logo (SVG, paths only: nanosvg ignores `<text>`) | `/userdata/system/wasmcart/theme/wasmcart.svg` |
| system definition | `/userdata/system/configs/emulationstation/es_systems_wasmcart.cfg` |
| emulator/core mapping (unknown systems otherwise fail with `MissingEmulator`) | `wasmcart.emulator=libretro` and `wasmcart.core=wasmcart` in `/userdata/system/batocera.conf` |
| boot-time symlinks | `/userdata/system/services/wasmcart`, enabled with `batocera-services enable wasmcart` |

## Verifying without a controller

Batocera's ES has an HTTP API on port 1234:

```sh
curl -s http://127.0.0.1:1234/systems | grep -c '"name": "wasmcart"'      # 1
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:1234/systems/wasmcart/logo   # 200 = theme found our SVG
curl -X POST http://127.0.0.1:1234/launch -d /userdata/roms/wasmcart/game.wasc
batocera-es-swissknife --emukill
```

RetroArch's log goes to `/userdata/system/logs/es_launch_stderr.log`; the core's
own log is written next to the cart as `wasmcart.log`. Do not stop the
`S31emulationstation` init script to test: it also stops the Wayland compositor,
and `emulatorlauncher` then fails in `getCurrentResolution`.

## Notes

- RetroArch is told `gfxbackend` per system; the default (`gl`) is right for the
  Pi. The core requests a GLES3 context and gets one from Mesa V3D.
- The core option `wasmcart_resolution` is honoured (`wasmcart.retroarchcore.wasmcart_resolution=...`
  in batocera.conf).
- Save data goes to `/userdata/saves/wasmcart/<cart>.srm` via RetroArch SRAM.
