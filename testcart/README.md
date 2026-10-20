# The test cart

`testcart.wasc` is wasmcart's test cart, built from
[wasmcart's `testcart/`](https://github.com/wasmcart/wasmcart/tree/main/testcart)
(source, build script and what it shows are there). The core compiles it in
(`cmake/embed.cmake`) and runs it when RetroArch starts the core with no content
(Load Core, then Start Core): four pads with every button, stick and trigger, the
pointer and keyboard, a note per face button with L/R panning, and a save
checked byte for byte at every start.

To update it, build it in wasmcart (`node testcart/build.mjs`) and copy the
`.wasc` here.
