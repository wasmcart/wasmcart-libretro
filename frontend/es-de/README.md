# ES-DE / EmulationStation integration

Adds a **"wasmcart"** system to ES-DE (and RetroDeck) so `.wasc` carts show up as
their own category, launched through the wasmcart-libretro core.

## Install

1. **Add the system** — copy `es_systems.xml` into ES-DE's `custom_systems/` dir
   (the supported user-add path; ES-DE does **not** overwrite it on update):
   - Native ES-DE: `~/ES-DE/custom_systems/es_systems.xml`
   - RetroDeck: `~/retrodeck/ES-DE/custom_systems/es_systems.xml`

   If the core is not installed where `%CORE_RETROARCH%` points (e.g. RetroDeck's
   read-only `/app` cores dir), change `<command>` to an absolute `-L` path to the
   `.so`, e.g. on RetroDeck:
   ```
   <command>%EMULATOR_RETROARCH% -L /home/<user>/retrodeck/storage/wasmcart-cores/wasmcart_libretro.so %ROM%</command>
   ```

2. **Add the logo** (optional but recommended) — drop `logos/wasmcart.svg` into the
   active theme's per-system logo dir. For art-book-next (RetroDeck default):
   ```
   ~/retrodeck/ES-DE/themes/art-book-next-es-de/_inc/systems/logos/wasmcart.svg
   ```
   (Theme files DO get overwritten on a theme update; re-copy after updating a theme.)

3. **Put carts** in `%ROMPATH%/wasmcart/` as `.wasc` files.

4. **Restart ES-DE/RetroDeck fully.** The systems config is read once at startup —
   "reload gamelist" is not enough; the new system only appears after a full restart.

## Why `<theme>wasmcart</theme>` (not `pc` or `ports`)

ES-DE picks a system's displayed **name and logo from the theme**, keyed by the
`<theme>` value (`${system.theme}`), NOT from `<fullname>`. Using a name **no stock
theme defines** (`wasmcart`) makes ES-DE fall back to our `<fullname>` and our own
dropped-in `logos/wasmcart.svg` (the `system-logo` element overlays
`${system.theme}.svg`, which `${logoSource}` resolves to `_inc/systems/logos/`).

## Logo gotcha: ES-DE renders SVGs with **nanosvg**, which ignores `<text>`

`logos/wasmcart.svg` must be built from **paths/shapes only** — `<text>` elements are
**not rendered** by ES-DE's SVG loader. The logo is pixel-block letters drawn as
`<rect>`s, white-filled so the theme can recolor it via `${systemLogoColor}`.
Regenerate with `python3 gen-logo.py > logos/wasmcart.svg` if the wordmark changes.
