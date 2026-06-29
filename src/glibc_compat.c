// glibc_compat.c — pin newer-glibc math symbols to old versions for portability.
//
// glibc 2.43 gave several float math functions "correctly rounded" variants with
// a fresh default symbol version (e.g. sqrtf@GLIBC_2.43). When the core is linked
// on a 2.43 host, references resolve to the 2.43 version and the .so then refuses
// to load on older runtimes (the RetroDECK / KDE flatpak runtime ships an older
// glibc):
//   version `GLIBC_2.43' not found (required by wasmcart_libretro.so)
//
// We use the linker's --wrap (set in CMakeLists): every reference to e.g. sqrtf —
// including ones inside the prebuilt libnode/native archives — is redirected to
// __wrap_sqrtf below, which forwards to the old @GLIBC_2.2.5 implementation (still
// present in every modern libm). Behaviour is identical for our uses; we only
// forgo 2.43's last-ulp rounding guarantee. Linux/glibc only.
#if defined(__linux__) && defined(__GLIBC__)

// Bind these names to the old, widely-available symbol versions.
__asm__(".symver __old_sqrtf,sqrtf@GLIBC_2.2.5");

extern float __old_sqrtf(float);

float __wrap_sqrtf(float x) { return __old_sqrtf(x); }
#endif
