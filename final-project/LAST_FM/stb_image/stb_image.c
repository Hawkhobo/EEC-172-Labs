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
//   stbi__jpeg x1 (test mallocs then frees LIFO, load reuses same slot):18 424
//   raw_data Y/Cb/Cr + coeff Y/Cb/Cr + alignment:                       18 528
//   linebuf x3:                                                             216
//   output pixels (64*64*3):                                            12 296
//   Total progressive:                                                  49 464  (48.3 KB)
//   Pool chosen:                                                        52 224  (51 KB, +margin)
//
// WHY NOT 69 KB ANYMORE?
//   The original bump allocator treated free() as a no-op, so stbi__jpeg_test
//   and stbi__jpeg_load each consumed a separate ~18 KB slot.  The allocator
//   below tracks s_stbi_pool_prev (cursor before the most recent malloc).
//   When free(p) is called and p IS the most recently allocated block, the
//   cursor is restored to s_stbi_pool_prev ("last-allocation pop").  For any
//   other pointer the free is a no-op (standard bump behavior).
//   stbi__jpeg_test does exactly one malloc + one LIFO free, so its slot is
//   reclaimed and stbi__jpeg_load reuses it -- saving one full stbi__jpeg
//   worth of pool (~18 KB).
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
#define STBI_POOL_SIZE  52224u   /* 51 KB -- see size calculation above */

static unsigned char s_stbi_pool[STBI_POOL_SIZE];
static unsigned int  s_stbi_pool_used = 0u;
static unsigned int  s_stbi_pool_prev = 0u;  /* cursor before most recent malloc */

static void *stbi_pool_malloc(size_t sz)
{
    unsigned int aligned;
    aligned = (s_stbi_pool_used + 7u) & ~7u;   /* 8-byte align */
    if (aligned + (unsigned int)sz > STBI_POOL_SIZE) {
        return NULL;                             /* pool exhausted */
    }
    s_stbi_pool_prev = s_stbi_pool_used;        /* remember pre-alloc cursor */
    s_stbi_pool_used = aligned + (unsigned int)sz;
    return (void *)(s_stbi_pool + aligned);
}

static void stbi_pool_free(void *p)
{
    unsigned int last_start;
    /* If p is the most recently allocated block (LIFO), pop the cursor back.
     * This reclaims the stbi__jpeg_test allocation so stbi__jpeg_load can
     * reuse the same pool slot.  Any other free is a no-op. */
    last_start = (s_stbi_pool_prev + 7u) & ~7u;
    if (p && (unsigned char *)p == s_stbi_pool + last_start) {
        s_stbi_pool_used = s_stbi_pool_prev;
    }
}

/* stbi_pool_realloc is only invoked on the PNG/zlib path, which is
 * compiled out by STBI_ONLY_JPEG.  Provided as a safety fallback. */
/* static void *stbi_pool_realloc(void *p, size_t newsz)
{
    void *q = stbi_pool_malloc(newsz);
    if (q && p) {
        memcpy(q, p, newsz);
    }
    return q;
} */

/* Call this after stbi_image_free() returns (i.e. after the draw loop in
 * oled_ui_render_album_jpeg).  Resets the pool so the next decode can reuse
 * all 51 KB from the beginning. */
void stbi_pool_reset(void)
{
    s_stbi_pool_used = 0u;
    s_stbi_pool_prev = 0u;
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
