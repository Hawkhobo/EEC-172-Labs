// stb_image implementation for CC3200 FM Radio Explorer
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// WHY A STATIC POOL?
// ------------------
// stb_image's JPEG decoder calls malloc() for two large allocations before
// any pixel data is touched:
//
//   1. stbi__jpeg_test()  -- malloc(sizeof(stbi__jpeg)) = ~18 KB to probe
//      the image format.  Frees it before returning.
//   2. stbi__jpeg_load()  -- malloc(sizeof(stbi__jpeg)) = ~18 KB again for
//      the actual decode.
//
// The CC3200 libc heap is shared with the SimpleLink host driver and is
// typically depleted to the point where an 18 KB contiguous allocation
// fails.  When malloc() returns NULL in stbi__jpeg_test(), the JPEG format
// test "fails" silently (returns 0) and stbi__load_main() falls through
// all format tests, ultimately returning "unknown image type" even for
// a byte-perfect JPEG starting with FF D8.
//
// SOLUTION: Replace malloc/free/realloc with a bump allocator backed by a
// static pool (s_stbi_pool[]).  stb_image then makes ZERO heap allocations.
// Call stbi_pool_reset() after stbi_image_free() to reclaim the entire pool
// for the next decode.
//
// POOL SIZE CALCULATION (64x64 JPEG, worst-case progressive, 8-byte align):
//   stbi__jpeg x2 (test + load, bump so both counted): 36 848
//   raw_data Y/Cb/Cr + coeff Y/Cb/Cr + alignment:      18 528
//   linebuf x3:                                            216
//   output pixels (64*64*3):                           12 296
//   Total progressive:                                 67 888  (66.3 KB)
//   Pool chosen:                                       70 656  (69 KB, +margin)
//
// Compile-time configuration:
//   STBI_ONLY_JPEG    - strip all non-JPEG decoders
//   STBI_NO_STDIO     - no FILE* I/O
//   STBI_NO_SIMD      - no SSE2/NEON (CC3200 Cortex-M4 has neither)
//   STBI_NO_LINEAR    - no float-per-channel output
//   STBI_NO_HDR       - no Radiance HDR format
//   STBI_NO_THREAD_LOCALS - no __thread / _Thread_local
//   STBI_ASSERT       - suppress assert.h; failures surface via NULL return

#include <string.h>   /* memcpy - needed by stbi_pool_realloc */

/* ---------------------------------------------------------------------------
 * Static bump-allocator pool
 * ------------------------------------------------------------------------- */
#define STBI_POOL_SIZE  70656u   /* 69 KB -- see size calculation above */

static unsigned char s_stbi_pool[STBI_POOL_SIZE];
static unsigned int  s_stbi_pool_used = 0u;

static void *stbi_pool_malloc(size_t sz)
{
    unsigned int aligned;
    aligned = (s_stbi_pool_used + 7u) & ~7u;   /* 8-byte align */
    if (aligned + (unsigned int)sz > STBI_POOL_SIZE) {
        return NULL;                             /* pool exhausted */
    }
    s_stbi_pool_used = aligned + (unsigned int)sz;
    return (void *)(s_stbi_pool + aligned);
}

static void stbi_pool_free(void *p)
{
    (void)p;    /* bump allocator -- individual frees are no-ops */
}

/* stbi_pool_realloc is only invoked on the PNG/zlib path, which is
 * compiled out by STBI_ONLY_JPEG.  Provided as a safety fallback. */
static void *stbi_pool_realloc(void *p, size_t newsz)
{
    void *q = stbi_pool_malloc(newsz);
    if (q && p) {
        memcpy(q, p, newsz);
    }
    return q;
}

/* Call this after stbi_image_free() returns (i.e. after the draw loop in
 * oled_ui_render_album_jpeg).  Resets the pool so the next decode can reuse
 * all 69 KB from the beginning. */
void stbi_pool_reset(void)
{
    s_stbi_pool_used = 0u;
}

/* Returns the number of pool bytes consumed so far (useful for diagnostics). */
unsigned int stbi_pool_used_bytes(void)
{
    return s_stbi_pool_used;
}

/* ---------------------------------------------------------------------------
 * Wire the pool into stb_image BEFORE including the implementation
 * ------------------------------------------------------------------------- */
#define STBI_MALLOC(sz)       stbi_pool_malloc(sz)
#define STBI_FREE(p)          stbi_pool_free(p)
#define STBI_REALLOC(p,newsz) stbi_pool_realloc((p),(newsz))

/* Decoder selection */
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x) ((void)(x))
#define STB_IMAGE_IMPLEMENTATION

#include "stb_image.h"
