// stb_image implementation for CC3200 FM Radio Explorer
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// Compile-time configuration:
//   STBI_ONLY_JPEG    - strip all non-JPEG decoders (PNG, BMP, TGA, GIF, ...)
//   STBI_NO_STDIO     - no FILE* I/O; we feed raw bytes from s_jpeg_buf
//   STBI_NO_SIMD      - CC3200 is ARM Cortex-M4; no NEON, no SSE2
//   STBI_NO_LINEAR    - no float-per-channel output
//   STBI_NO_HDR       - no Radiance HDR format
//   STBI_ASSERT       - suppress assert.h; failures surface via NULL return
//
// Memory footprint (heap, peak during decode):
//   48x48 progressive JPEG: ~36 KB  (freed immediately after draw)
//   64x64 progressive JPEG: ~48 KB  (freed immediately after draw)
//   All allocations are freed by stbi_image_free() before returning to caller.

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)(x))
#define STB_IMAGE_IMPLEMENTATION

#include "stb_image.h"
