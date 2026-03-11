// Graphical UI OLED API
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "common.h"

// Adafruit driver layer
#include "../adafruit_oled_lib/Adafruit_GFX.h"
#include "../adafruit_oled_lib/Adafruit_SSD1351.h"

#include "oled_ui.h"
#include "../LAST_FM/lastfm.h"

// ===========================================================================
// JPEG album-cover support  (compiled in only when LASTFM_ENABLE_JPEG is set)
// ===========================================================================
// oled_ui.c owns everything that touches pixels during JPEG decode:
//   - TJpgDec work buffer
//   - RGB888 to RGB565 conversion macro
//   - Scale parameter computation (aspect-ratio preserving, letterbox)
//   - jpeg_out_cb: TJpgDec output callback -> drawPixel()
//   - oled_ui_render_album_jpeg(): public entry point called by lastfm.c
//
// lastfm.c owns the network side:
//   - Opening the TLS socket to the CDN
//   - Skipping the HTTP response headers
//   - jpeg_in_cb: TJpgDec input callback that streams bytes from the socket
// ===========================================================================

#include "../LAST_FM/tjpgdec/tjpgd.h"

// ===========================================================================
// Layout constants
// ===========================================================================

#define SCREEN_W          128
#define SCREEN_H          128

// Banner (top navigation bar)
#define BANNER_H           12   // pixels tall (includes 1-px bottom divider)
#define BANNER_ITEM_W      18   // px per item  (7 × 18 = 126; 1-px left margin)
#define BANNER_X_MARGIN     1   // left margin before first item

// Content area (below the banner)
#define CONTENT_Y        BANNER_H
#define CONTENT_H        (SCREEN_H - BANNER_H)

// Font geometry (Adafruit 5×7 glcdfont at size-1)
#define CHAR_W             6    // 5px glyph + 1px gap
#define CHAR_H             8
#define LINE_H             9    // CHAR_H + 1px row gap
#define CHARS_PER_LINE    21    // floor(128 / 6)

// how many text lines fit in the content area
#define CONTENT_MAX_LINES  (CONTENT_H / LINE_H)

// RGB888 (3 bytes) -> RGB565 (16-bit) for the SSD1351
#define RGB888_TO_565(r, g, b) \
    ((unsigned int)( (((unsigned int)(r) & 0xF8u) << 8) | \
                     (((unsigned int)(g) & 0xFCu) << 3) | \
                     (((unsigned int)(b) & 0xF8u) >> 3) ))

// TJpgDec work area — 3.5 KB comfortably covers 16×16 MCU blocks.
#define _JPGDEC_WORK_SZ  3500
static unsigned char s_jpgdec_work[_JPGDEC_WORK_SZ];

// Scaling parameters computed by compute_scale() and used by jpeg_out_cb().
typedef struct {
    int src_w, src_h;   // decoded image dimensions (after TJpgDec scale factor)
    int dst_x, dst_y;   // top-left offset within the content area (letterbox)
    int dst_w, dst_h;   // final pixel dimensions written to the OLED
} _ScaleParams;

static _ScaleParams s_scale;

// ---------------------------------------------------------------------------
// compute_scale
//
// Fills s_scale so that a src_w × src_h image is fitted inside the content
// area (SCREEN_W × CONTENT_H) with the aspect ratio preserved.
// Any unused border (letterbox / pillar) is already black from the fillRect
// call in oled_ui_render_album_jpeg().
// ---------------------------------------------------------------------------
static void compute_scale(int src_w, int src_h)
{
    // Choose the scale that fills the smaller dimension exactly:
    //   ratio_x = SCREEN_W  / src_w
    //   ratio_y = CONTENT_H / src_h
    //   use whichever ratio is smaller (fit-within, not crop)
    //
    // Integer comparison: ratio_x <= ratio_y
    //   <=>  SCREEN_W * src_h <= CONTENT_H * src_w
    int dst_w, dst_h;
    if (SCREEN_W * src_h <= CONTENT_H * src_w) {
        // Width-constrained: fill full width
        dst_w = SCREEN_W;
        dst_h = (src_h * SCREEN_W) / src_w;
    } else {
        // Height-constrained: fill full content height
        dst_h = CONTENT_H;
        dst_w = (src_w * CONTENT_H) / src_h;
    }

    s_scale.src_w = src_w;
    s_scale.src_h = src_h;
    s_scale.dst_w = dst_w;
    s_scale.dst_h = dst_h;
    // Centre the image within the content area
    s_scale.dst_x = (SCREEN_W  - dst_w) / 2;
    s_scale.dst_y = (CONTENT_H - dst_h) / 2;
}

// ---------------------------------------------------------------------------
// jpeg_out_cb — TJpgDec output callback
//
// Called by jd_decomp() for each decoded MCU block (up to 16×16 pixels).
// Converts RGB888 -> RGB565 and maps source pixels onto the OLED content
// area using integer nearest-neighbour scaling.
// ---------------------------------------------------------------------------
static int jpeg_out_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    uint8_t *rgb = (uint8_t *)bitmap;
    int sy, sx;
    uint8_t r, g, b;

    for (sy = rect->top; sy <= rect->bottom; sy++) {
        for (sx = rect->left; sx <= rect->right; sx++) {
            r = *rgb++;
            g = *rgb++;
            b = *rgb++;

            // Nearest-neighbour map from source pixel to OLED pixel
            int dx = s_scale.dst_x + (sx * s_scale.dst_w) / s_scale.src_w;
            int dy = CONTENT_Y + s_scale.dst_y +
                     (sy * s_scale.dst_h) / s_scale.src_h;

            if (dx >= 0 && dx < SCREEN_W &&
                dy >= CONTENT_Y && dy < SCREEN_H) {
                drawPixel(dx, dy, RGB888_TO_565(r, g, b));
            }
        }
    }
    return 1;  // JDR_OK — continue decoding
}

// ---------------------------------------------------------------------------
// oled_ui_render_album_jpeg — public implementation
//
// See oled_ui.h for the full contract.
// ---------------------------------------------------------------------------
int oled_ui_render_album_jpeg(OledJpegInFn in_fn, void *device_ctx)
{
    JDEC         jd;
    JRESULT      jres;
    uint8_t      scale;
    unsigned int dec_w;
    unsigned int dec_h;

    // Clear the content area to black first (letterbox margins stay black)
    fillRect(0u, (unsigned int)CONTENT_Y,
             (unsigned int)SCREEN_W, (unsigned int)CONTENT_H, 0x0000u);

    jres = jd_prepare(&jd,
                      (size_t(*)(JDEC *, uint8_t *, size_t))in_fn,
                      s_jpgdec_work, _JPGDEC_WORK_SZ,
                      device_ctx);
    if (jres != JDR_OK) {
        UART_PRINT("[OLED] jd_prepare failed (%d)\n\r", (int)jres);
        return -8;  // LASTFM_ERR_JPEG, avoid including lastfm.h here
    }

    UART_PRINT("[OLED] JPEG %u x %u\n\r", jd.width, jd.height);

    // Choose TJpgDec hardware scale factor:
    //   scale=0 -> full resolution
    //   scale=1 -> 1/2 in each dimension  (use for images >= 128 px wide)
    //   scale=2 -> 1/4                    (use for very large images >= 512 px)
    // After TJpgDec scaling, compute_scale() handles the final NN step.
    scale = 0;
    if (jd.width >= 512) scale = 2;

    // Decoded dimensions after TJpgDec halving
    dec_w = jd.width  >> scale;
    dec_h = jd.height >> scale;
    compute_scale((int)dec_w, (int)dec_h);

    jres = jd_decomp(&jd, jpeg_out_cb, scale);
    if (jres != JDR_OK) {
        UART_PRINT("[OLED] jd_decomp failed (%d)\n\r", (int)jres);
        return -8;  // LASTFM_ERR_JPEG
    }

    UART_PRINT("[OLED] Album art rendered (%dx%d -> %dx%d at +%d,+%d)\n\r",
               (int)dec_w, (int)dec_h,
               s_scale.dst_w, s_scale.dst_h,
               s_scale.dst_x, s_scale.dst_y);
    return 0;  // LASTFM_OK
}

// ===========================================================================
// Color aliases
// ===========================================================================
#define BLACK   0x0000u
#define WHITE   0xFFFFu
#define CYAN    0x07FFu
#define YELLOW  0xFFE0u
#define GREEN   0x07E0u
#define BLUE    0x001Fu
#define RED     0xF800u
#define MAGENTA 0xF81F
#define GREY    0x8410u
#define DARK_GREY 0x39E7u

// UI colour scheme - RETHEME ELEMENTS HERE!
#define COL_BANNER_BG       BLACK
#define COL_BANNER_ITEM     WHITE
#define COL_BANNER_ACTIVE   CYAN
#define COL_BANNER_ACT_TXT  BLACK
#define COL_DIVIDER         WHITE
#define COL_LABEL           YELLOW
#define COL_VALUE           WHITE
#define COL_MUTED           GREY
#define COL_PROGRESS_FILL   GREEN
#define COL_PROGRESS_EMPTY  DARK_GREY
#define COL_SIGNAL_BAR      BLUE
#define COL_SECTION_BG      DARK_GREY
#define COL_UNAVAILABLE     RED
#define COL_SCROLL_IND      GREY

// ===========================================================================
// Internal state
// ===========================================================================
static OledViewID g_view = OLED_VIEW_RADIO;

// Per-view scroll position.
//   List views: index of first item to display (0-based)
//   Text views: index of first wrapped line to display (0-based)
static int g_scroll[OLED_VIEW_COUNT];

// View data stores
static RadioViewData  g_radio;
static AlbumCoverData g_album;
static ListViewData   g_similar_artists;
static ArtistBioData  g_artist_bio;
static ListViewData   g_genre_tags;
static ListViewData   g_similar_tracks;
static LyricsData     g_lyrics;

// 3-char banner abbreviations (corresponds to OledViewId)
static const char *k_banner_labels[OLED_VIEW_COUNT] = {
    "RAD",
    "ALB",
    "ART",
    "BIO",
    "GNR",
    "TRK",
    "LYR"
};

// ===========================================================================
// Low-level drawing helpers (all static — internal use only)
// ===========================================================================

// Draw a null-terminated ASCII string starting at pixel (x, y)
static void ui_str(int x, int y, const char *s,
                   unsigned int fg, unsigned int bg, unsigned char sz)
{
    while (*s) {
        drawChar(x, y, (unsigned char)*s++, fg, bg, sz);
        x += CHAR_W * (int)sz;
    }
}

// Clear a rectangular region to black
static void ui_clear_rect(int x, int y, int w, int h)
{
    fillRect((unsigned int)x, (unsigned int)y,
             (unsigned int)w, (unsigned int)h, BLACK);
}

// Draw a filled horizontal progress bar.
//   (x,y) = top-left corner, w/h = total bar size, pct = 0..100
static void ui_progress_bar(int x, int y, int w, int h, int pct)
{
    int fill;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    fill = (w * pct) / 100;
    if (fill > 0)
        fillRect((unsigned int)x, (unsigned int)y,
                 (unsigned int)fill, (unsigned int)h, COL_PROGRESS_FILL);
    if (fill < w)
        fillRect((unsigned int)(x + fill), (unsigned int)y,
                 (unsigned int)(w - fill), (unsigned int)h, COL_PROGRESS_EMPTY);
}

// Draw 4 signal-strength bars
// Bars grow in height from left to right.  strength = 0..100
static void ui_signal_bars(int x, int y, int strength)
{
    int filled_bars, i;
    if (strength < 0)   strength = 0;
    if (strength > 100) strength = 100;
    filled_bars = (strength * 4 + 50) / 100;   // round to 0-4 bars

    for (i = 0; i < 4; i++) {
        int bh = 3 + i * 2;                     // heights: 3, 5, 7, 9
        int bx = x + i * 5;
        int by = y + (9 - bh);                  // align bottoms
        unsigned int col = (i < filled_bars) ? COL_SIGNAL_BAR : DARK_GREY;
        fillRect((unsigned int)bx, (unsigned int)by,
                 3u, (unsigned int)bh, col);
    }
}

// Draw small scroll-indicator dots on the right edge of the screen.
//   show_up / show_down  indicate whether more content exists in that direction.
static void ui_scroll_indicators(bool show_up, bool show_down)
{
    if (show_up) {
        // Two-pixel dot at top-right of content area
        fillRect(125u, (unsigned int)(CONTENT_Y + 2), 2u, 4u, COL_SCROLL_IND);
    }
    if (show_down) {
        // Two-pixel dot at bottom-right of screen
        fillRect(125u, (unsigned int)(SCREEN_H - 6), 2u, 4u, COL_SCROLL_IND);
    }
}

// ===========================================================================
// Text wrapping helpers
// ===========================================================================

// Compute how many columns to consume for the current line, respecting:
//   - Hard newline '\n'
//   - Maximum width max_chars
//   - Soft word-wrap (back up to last space if mid-word)
// *advance is set to how many characters to skip in the source string
// (may differ from returned count if we backed up over a space).
static int ui_line_fit(const char *ptr, int max_chars, int *advance)
{
    int avail = 0;

    // Count until newline, end-of-string, or column limit
    while (ptr[avail] && ptr[avail] != '\n' && avail < max_chars)
        avail++;

    // Soft break: if we hit the column limit mid-word, back up
    if (ptr[avail] != '\0' && ptr[avail] != '\n' && avail == max_chars) {
        int j;
        for (j = avail - 1; j > 0; j--) {
            if (ptr[j] == ' ') { avail = j; break; }
        }
    }

    *advance = avail;
    if (ptr[avail] == '\n' || ptr[avail] == ' ')
        (*advance)++;    // consume the delimiter without printing it

    return avail;        // printable character count
}

// Count the total number of wrapped lines that text produces.
static int ui_count_lines(const char *text)
{
    int lines = 0;
    const char *ptr = text;
    if (!text || !*text) return 0;
    while (*ptr) {
        int advance;
        ui_line_fit(ptr, CHARS_PER_LINE, &advance);
        lines++;
        ptr += advance;
    }
    return lines;
}

// Render a block of word-wrapped text inside the content area.
//
//   text        — null-terminated source string ('\n' forces a line break)
//   y_start     — top pixel of first visible line (must be >= CONTENT_Y)
//   skip_lines  — number of leading wrapped lines to skip (scroll offset)
//   fg          — text foreground colour
//
// Returns the TOTAL number of wrapped lines in the text (not just those
// drawn), so the caller can compute scroll limits.
static int ui_render_text_block(const char *text, int y_start,
                                int skip_lines, unsigned int fg)
{
    int   total_lines = 0;
    int   y = y_start;
    const char *ptr;
    char  buf[CHARS_PER_LINE + 1];

    if (!text || !*text) return 0;
    ptr = text;

    while (*ptr) {
        int advance, fit;
        fit = ui_line_fit(ptr, CHARS_PER_LINE, &advance);

        if (total_lines >= skip_lines) {
            if (y + CHAR_H <= SCREEN_H) {
                // Clear this text row then draw
                ui_clear_rect(0, y, SCREEN_W - 3, LINE_H); // leave 3px for indicator
                strncpy(buf, ptr, (unsigned int)fit);
                buf[fit] = '\0';
                ui_str(0, y, buf, fg, BLACK, 1);
                y += LINE_H;
            }
        }

        total_lines++;
        ptr += advance;
    }

    return total_lines;
}

// ===========================================================================
// Banner renderer
// ===========================================================================
static void render_banner(void)
{
    int i;

    // Background strip
    fillRect(0u, 0u, (unsigned int)SCREEN_W, (unsigned int)(BANNER_H - 1),
             COL_BANNER_BG);

    for (i = 0; i < OLED_VIEW_COUNT; i++) {
        int ix = BANNER_X_MARGIN + i * BANNER_ITEM_W;

        if (i == (int)g_view) {
            // Highlight the active tab
            fillRect((unsigned int)ix, 1u,
                     (unsigned int)BANNER_ITEM_W, (unsigned int)(BANNER_H - 2),
                     COL_BANNER_ACTIVE);
            ui_str(ix, 2, k_banner_labels[i],
                   COL_BANNER_ACT_TXT, COL_BANNER_ACTIVE, 1);
        } else {
            ui_str(ix, 2, k_banner_labels[i],
                   COL_BANNER_ITEM, COL_BANNER_BG, 1);
        }
    }

    // Bottom divider
    drawFastHLine(0, BANNER_H - 1, SCREEN_W, COL_DIVIDER);
}

// ===========================================================================
// View 0 — Radio
// ===========================================================================

// Copy up to n chars from src; append "..." if truncated.
static void ui_trunc(char *dst, const char *src, int n)
{
    int len = (int)strlen(src);
    if (len <= n) {
        strcpy(dst, src);
    } else {
        strncpy(dst, src, (unsigned int)(n - 3));
        dst[n - 3] = '.'; dst[n - 2] = '.'; dst[n - 1] = '.'; dst[n] = '\0';
    }
}

static void render_radio_view(void)
{
    char buf[CHARS_PER_LINE + 4];
    int  y = CONTENT_Y + 1;

    ui_clear_rect(0, CONTENT_Y, SCREEN_W, CONTENT_H);

    // ---- Row 0: station / callsign highlight bar ----
    fillRect(0u, (unsigned int)y, (unsigned int)SCREEN_W,
             (unsigned int)LINE_H, COL_BANNER_ACTIVE);
    snprintf(buf, sizeof(buf), "%-8s  %s",
             g_radio.station, g_radio.callsign);
    buf[CHARS_PER_LINE] = '\0';
    ui_str(2, y, buf, COL_BANNER_ACT_TXT, COL_BANNER_ACTIVE, 1);
    y += LINE_H + 3;

    // ---- Row 1: "Now Playing" label ----
    ui_str(0, y, "Now Playing:", COL_LABEL, BLACK, 1);
    y += LINE_H;

    // ---- Row 2-3: Song title (up to 2 lines, then truncated) ----
    if (g_radio.song[0] != '\0') {
        // Line A
        int slen = (int)strlen(g_radio.song);
        if (slen <= CHARS_PER_LINE) {
            ui_str(0, y, g_radio.song, COL_VALUE, BLACK, 1);
            y += LINE_H;
        } else {
            // Find soft-wrap point for first line
            int fit1 = CHARS_PER_LINE;
            int j;
            for (j = CHARS_PER_LINE - 1; j > 0; j--) {
                if (g_radio.song[j] == ' ') { fit1 = j; break; }
            }
            strncpy(buf, g_radio.song, (unsigned int)fit1);
            buf[fit1] = '\0';
            ui_str(0, y, buf, COL_VALUE, BLACK, 1);
            y += LINE_H;

            // Line B (truncated)
            const char *rest = g_radio.song + fit1;
            if (*rest == ' ') rest++;
            ui_trunc(buf, rest, CHARS_PER_LINE);
            ui_str(0, y, buf, COL_VALUE, BLACK, 1);
            y += LINE_H;
        }
    } else {
        ui_str(0, y, "(No RDS data)", COL_MUTED, BLACK, 1);
        y += LINE_H;
    }

    y += 2;

    // ---- Row 4-5: Artist ----
    ui_str(0, y, "Artist:", COL_LABEL, BLACK, 1);
    y += LINE_H;

    if (g_radio.artist[0] != '\0') {
        ui_trunc(buf, g_radio.artist, CHARS_PER_LINE);
        ui_str(0, y, buf, COL_VALUE, BLACK, 1);
    } else {
        ui_str(0, y, "(Unknown)", COL_MUTED, BLACK, 1);
    }
    y += LINE_H + 4;

    // ---- Row 6: Track progress bar ----
    ui_str(0, y, "Progress:", COL_LABEL, BLACK, 1);
    y += LINE_H;
    ui_progress_bar(0, y, 110, 6, g_radio.progress_pct);
    snprintf(buf, sizeof(buf), "%3d%%", g_radio.progress_pct);
    ui_str(112, y - 1, buf, COL_VALUE, BLACK, 1);
    y += 10;

    // ---- Row 7: Signal strength ----
    ui_str(0, y, "Signal:", COL_LABEL, BLACK, 1);
    ui_signal_bars(50, y, g_radio.signal_strength);
    snprintf(buf, sizeof(buf), "%3d%%", g_radio.signal_strength);
    ui_str(72, y, buf, COL_MUTED, BLACK, 1);
}

// ===========================================================================
// View 1 — Album Cover
// ===========================================================================
static void render_album_cover_view(void)
{
    ui_clear_rect(0, CONTENT_Y, SCREEN_W, CONTENT_H);

    if (!g_album.available) {
        // Placeholder: outlined box with crossed diagonals
        int bx = 14, by = CONTENT_Y + 8, bw = 100, bh = 80;
        drawRect(bx, by, bw, bh, GREY);
        drawLine(bx,        by,        bx + bw - 1, by + bh - 1, DARK_GREY);
        drawLine(bx + bw - 1, by,      bx,          by + bh - 1, DARK_GREY);
        ui_str(20, by + bh + 6,  "Album art loading", COL_MUTED, BLACK, 1);
        ui_str(28, by + bh + 16, "or unavailable.",   COL_MUTED, BLACK, 1);
    } else {
        // Art URL is cached — oled_ui_render_album_jpeg() will overwrite this
        // area immediately after oled_ui_render() returns.  Show a brief
        // "loading" indicator so the display is not blank during the fetch.
        int cy = CONTENT_Y + (CONTENT_H / 2) - CHAR_H;
        ui_str(16, cy,          "Loading album art", COL_LABEL, BLACK, 1);
        ui_str(34, cy + LINE_H, "Please wait...",   COL_MUTED, BLACK, 1);
    }
}

// ===========================================================================
// Shared helpers for list views (Views 2, 4, 5)
// ===========================================================================

// Render a section-header bar (dark background, yellow text).
// Returns y after the header.
static int ui_section_header(int y, const char *title)
{
    fillRect(0u, (unsigned int)y, (unsigned int)SCREEN_W,
             (unsigned int)LINE_H, COL_SECTION_BG);
    ui_str(2, y, title, COL_LABEL, COL_SECTION_BG, 1);
    return y + LINE_H + 2;
}

// Render one list entry (possibly wrapping over 2 lines).
// Returns updated y, or -1 if y has gone off-screen.
static int ui_render_list_item(int y, int number, const char *text)
{
    char line[CHARS_PER_LINE + 1];
    char full[CHARS_PER_LINE + 1 + 4]; // "nn. " prefix
    int  prefix_len;

    if (y + CHAR_H > SCREEN_H) return -1;

    snprintf(full, sizeof(full), "%2d. ", number);
    prefix_len = (int)strlen(full);
    strncat(full, text, (unsigned int)(CHARS_PER_LINE - prefix_len));
    full[CHARS_PER_LINE] = '\0';

    // Does the full string fit on one line?
    int total_len = (int)strlen(full);
    if (total_len <= CHARS_PER_LINE) {
        ui_clear_rect(0, y, SCREEN_W - 3, LINE_H);
        ui_str(0, y, full, COL_VALUE, BLACK, 1);
        return y + LINE_H;
    }

    // Soft-wrap: find last space within column limit
    int fit = CHARS_PER_LINE;
    int j;
    for (j = CHARS_PER_LINE - 1; j > prefix_len; j--) {
        if (full[j] == ' ') { fit = j; break; }
    }
    strncpy(line, full, (unsigned int)fit);
    line[fit] = '\0';
    ui_clear_rect(0, y, SCREEN_W - 3, LINE_H);
    ui_str(0, y, line, COL_VALUE, BLACK, 1);
    y += LINE_H;

    if (y + CHAR_H <= SCREEN_H) {
        const char *rest = full + fit;
        if (*rest == ' ') rest++;
        strncpy(line, rest, CHARS_PER_LINE);
        line[CHARS_PER_LINE] = '\0';
        ui_clear_rect(0, y, SCREEN_W - 3, LINE_H);
        ui_str(2, y, line, COL_MUTED, BLACK, 1);   // slightly indented continuation
        y += LINE_H;
    }

    return y;
}

// ===========================================================================
// Generic list view renderer (reused by Views 2, 4, 5)
// ===========================================================================
static void render_list_view(const ListViewData *data, const char *title)
{
    int y = CONTENT_Y + 1;
    int scroll = g_scroll[(int)g_view];
    int i;

    ui_clear_rect(0, CONTENT_Y, SCREEN_W, CONTENT_H);

    y = ui_section_header(y, title);

    if (data->count == 0) {
        ui_str(0, y, "(No data available)", COL_MUTED, BLACK, 1);
        return;
    }

    // Clamp scroll so we can't scroll past the last item
    if (scroll >= data->count) {
        scroll = data->count - 1;
        g_scroll[(int)g_view] = scroll;
    }

    // Render items starting from scroll offset
    for (i = scroll; i < data->count; i++) {
        int next_y = ui_render_list_item(y, i + 1, data->items[i]);
        if (next_y < 0) break;   // off-screen
        y = next_y;
    }

    // Scroll indicators
    ui_scroll_indicators(scroll > 0, i < data->count);
}

// ===========================================================================
// Generic long-text view renderer (reused by Views 3 and 6)
// ===========================================================================
static void render_text_view(const char *text, const char *title,
                              bool available, const char *unavail_msg)
{
    int y = CONTENT_Y + 1;
    int scroll = g_scroll[(int)g_view];

    ui_clear_rect(0, CONTENT_Y, SCREEN_W, CONTENT_H);
    y = ui_section_header(y, title);

    if (!available || !text || text[0] == '\0') {
        ui_str(0, y, unavail_msg ? unavail_msg : "(No data)", COL_UNAVAILABLE, BLACK, 1);
        return;
    }

    // Count total lines to clamp scroll and draw indicators
    int total = ui_count_lines(text);
    int visible = (SCREEN_H - y) / LINE_H;

    // Clamp scroll to prevent scrolling past the last line
    int max_scroll = total - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) {
        scroll = max_scroll;
        g_scroll[(int)g_view] = scroll;
    }

    ui_render_text_block(text, y, scroll, COL_VALUE);

    ui_scroll_indicators(scroll > 0, scroll < max_scroll);
}

// ===========================================================================
// Public API
// ===========================================================================
void oled_ui_init(void)
{
    int i;

    g_view = OLED_VIEW_RADIO;
    for (i = 0; i < OLED_VIEW_COUNT; i++)
        g_scroll[i] = 0;

    // Zero all data stores and populate placeholders
    memset(&g_radio,           0, sizeof(g_radio));
    memset(&g_album,           0, sizeof(g_album));
    memset(&g_similar_artists, 0, sizeof(g_similar_artists));
    memset(&g_artist_bio,      0, sizeof(g_artist_bio));
    memset(&g_genre_tags,      0, sizeof(g_genre_tags));
    memset(&g_similar_tracks,  0, sizeof(g_similar_tracks));
    memset(&g_lyrics,          0, sizeof(g_lyrics));

    strncpy(g_radio.station,  "---.- FM", UI_MAX_STATION_LEN  - 1);
    strncpy(g_radio.callsign, "----",     UI_MAX_CALLSIGN_LEN - 1);
    strncpy(g_radio.song,     "(No RDS)", UI_MAX_SONG_LEN     - 1);
    strncpy(g_radio.artist,   "(Unknown)",UI_MAX_ARTIST_LEN   - 1);

    g_album.available   = false;
    g_lyrics.available  = false;

    oled_ui_render();
}

// ---- Navigation ----
void oled_ui_navigate_left(void)
{
    if ((int)g_view == 0)
        g_view = (OledViewID)(OLED_VIEW_COUNT - 1);
    else
        g_view = (OledViewID)((int)g_view - 1);

    g_scroll[(int)g_view] = 0;   // reset scroll on view change
}

void oled_ui_navigate_right(void)
{
    g_view = (OledViewID)(((int)g_view + 1) % OLED_VIEW_COUNT);
    g_scroll[(int)g_view] = 0;
}

void oled_ui_set_view(OledViewID view)
{
    if ((unsigned int)view < (unsigned int)OLED_VIEW_COUNT) {
        g_view = view;
        g_scroll[(int)g_view] = 0;
    }
}

OledViewID oled_ui_get_view(void)
{
    return g_view;
}

void oled_ui_scroll_up(void)
{
    if (g_scroll[(int)g_view] > 0)
        g_scroll[(int)g_view]--;
}

void oled_ui_scroll_down(void)
{
    // Upper bound is enforced lazily inside each render function
    g_scroll[(int)g_view]++;
}

void oled_ui_reset_scroll(void)
{
    g_scroll[(int)g_view] = 0;
}

void oled_ui_render(void)
{
    render_banner();

    switch (g_view) {
        case OLED_VIEW_RADIO:
            render_radio_view();
            break;

        case OLED_VIEW_ALBUM_COVER:
            render_album_cover_view();
            break;

        case OLED_VIEW_SIMILAR_ARTISTS:
            render_list_view(&g_similar_artists, "Similar Artists");
            break;

        case OLED_VIEW_ARTIST_BIO:
            render_text_view(g_artist_bio.text, "Artist Biography",
                             g_artist_bio.text[0] != '\0',
                             "(No biography available)");
            break;

        case OLED_VIEW_GENRE_TAGS:
            render_list_view(&g_genre_tags, "Genre Tags");
            break;

        case OLED_VIEW_SIMILAR_TRACKS:
            render_list_view(&g_similar_tracks, "Similar Tracks");
            break;

        case OLED_VIEW_LYRICS:
            render_text_view(g_lyrics.text, "Song Lyrics",
                             g_lyrics.available,
                             "(Lyrics unavailable for this track)");
            break;

        default:
            break;
    }
}

void oled_ui_update_radio(const char *station, const char *callsign,
                           const char *song, const char *artist,
                           int progress_pct, int signal_strength)
{
    if (station)  strncpy(g_radio.station,  station,  UI_MAX_STATION_LEN  - 1);
    if (callsign) strncpy(g_radio.callsign, callsign, UI_MAX_CALLSIGN_LEN - 1);
    if (song)     strncpy(g_radio.song,     song,     UI_MAX_SONG_LEN     - 1);
    if (artist)   strncpy(g_radio.artist,   artist,   UI_MAX_ARTIST_LEN   - 1);
    g_radio.progress_pct    = progress_pct;
    g_radio.signal_strength = signal_strength;
}

void oled_ui_update_album_cover(bool available)
{
    g_album.available = available;
}

void oled_ui_update_similar_artists(const char artists[][UI_MAX_LIST_ITEM_LEN],
                                     int count)
{
    int i;
    if (count > UI_MAX_LIST_ITEMS) count = UI_MAX_LIST_ITEMS;
    g_similar_artists.count = count;
    for (i = 0; i < count; i++) {
        strncpy(g_similar_artists.items[i], artists[i],
                UI_MAX_LIST_ITEM_LEN - 1);
        g_similar_artists.items[i][UI_MAX_LIST_ITEM_LEN - 1] = '\0';
    }
}

void oled_ui_update_artist_bio(const char *bio)
{
    if (bio) {
        strncpy(g_artist_bio.text, bio, UI_MAX_BIO_LEN - 1);
        g_artist_bio.text[UI_MAX_BIO_LEN - 1] = '\0';
    }
}

void oled_ui_update_genre_tags(const char tags[][UI_MAX_LIST_ITEM_LEN], int count)
{
    int i;
    if (count > UI_MAX_LIST_ITEMS) count = UI_MAX_LIST_ITEMS;
    g_genre_tags.count = count;
    for (i = 0; i < count; i++) {
        strncpy(g_genre_tags.items[i], tags[i], UI_MAX_LIST_ITEM_LEN - 1);
        g_genre_tags.items[i][UI_MAX_LIST_ITEM_LEN - 1] = '\0';
    }
}

void oled_ui_update_similar_tracks(const char tracks[][UI_MAX_LIST_ITEM_LEN],
                                    int count)
{
    int i;
    if (count > UI_MAX_LIST_ITEMS) count = UI_MAX_LIST_ITEMS;
    g_similar_tracks.count = count;
    for (i = 0; i < count; i++) {
        strncpy(g_similar_tracks.items[i], tracks[i],
                UI_MAX_LIST_ITEM_LEN - 1);
        g_similar_tracks.items[i][UI_MAX_LIST_ITEM_LEN - 1] = '\0';
    }
}

void oled_ui_update_lyrics(bool available, const char *lyrics)
{
    g_lyrics.available = available;
    if (available && lyrics) {
        strncpy(g_lyrics.text, lyrics, UI_MAX_LYRICS_LEN - 1);
        g_lyrics.text[UI_MAX_LYRICS_LEN - 1] = '\0';
    } else {
        g_lyrics.text[0] = '\0';
    }
}

// ===========================================================================
// Diagnostics
// ===========================================================================
//
// oled_ui_draw_diagnostics()
//
// PURPOSE
//   Verify that the display dimensions and coordinate system match what the
//   UI code expects.  Call this once from main() *instead* of oled_ui_render()
//   to produce:
//     1. A visual calibration pattern on the OLED.
//     2. A UART0 log of every compile-time constant that controls layout.
//
// VISUAL PATTERN (what you should see if everything is correct on 128x128)
//   - Full-screen red border (pixels 0,0 to 127,127)
//   - Green cross-hairs at the exact centre (64,64)
//   - Blue filled rectangle covering rows 0-11 (the banner zone)
//   - White pixel at each corner: (0,0) (127,0) (0,127) (127,127)
//   - Yellow text "0,0" near top-left and "127,127" near bottom-right
//   - Cyan text row at y=CONTENT_Y showing the banner/content split line
//   - A column of magenta dots every 10 rows along x=0 (y=0,10,20,...,120)
//
// HOW TO INTERPRET A WRONG RESULT
//   - Pattern clipped on right/bottom   WIDTH/HEIGHT or SSD1351W/H too small
//   - Pattern only in top portion       HEIGHT constant wrong (e.g. =96 not 128)
//   - Axes swapped                      REMAP register 0x74 may need adjustment
//   - Corners not at expected coords    coordinate origin offset issue
//
// UART LOG (read in CCS console)
//   DIAG: SSD1351WIDTH  = <value>
//   DIAG: SSD1351HEIGHT = <value>
//   DIAG: WIDTH         = <value>   (GFX clip width)
//   DIAG: HEIGHT        = <value>   (GFX clip height)
//   DIAG: SCREEN_W      = 128
//   DIAG: SCREEN_H      = 128
//   DIAG: BANNER_H      = 12
//   DIAG: CONTENT_Y     = 12
//   DIAG: CONTENT_H     = 116
//   DIAG: LINE_H        = 9
//   DIAG: CHARS_PER_LINE= 21
//   DIAG: CONTENT_MAX_LINES = 12
//   DIAG: drawPixel test at (0,0) (127,0) (0,127) (127,127)
//   DIAG: done

void oled_ui_draw_diagnostics(void)
{
    char buf[48];
    int  i;

    // ------------------------------------------------------------------
    // 1. UART log of all layout-critical constants
    // ------------------------------------------------------------------
    UART_PRINT("\r\n===== oled_ui DIAGNOSTICS =====\r\n");

    snprintf(buf, sizeof(buf), "DIAG: SSD1351WIDTH       = %d\r\n", SSD1351WIDTH);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: SSD1351HEIGHT      = %d\r\n", SSD1351HEIGHT);
    UART_PRINT(buf);

    // WIDTH and HEIGHT come from Adafruit_GFX (used for clipping in drawChar)
    snprintf(buf, sizeof(buf), "DIAG: GFX WIDTH (clip)   = %d\r\n", width());
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: GFX HEIGHT (clip)  = %d\r\n", height());
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: SCREEN_W           = %d\r\n", SCREEN_W);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: SCREEN_H           = %d\r\n", SCREEN_H);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: BANNER_H           = %d\r\n", BANNER_H);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: CONTENT_Y          = %d\r\n", CONTENT_Y);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: CONTENT_H          = %d\r\n", CONTENT_H);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: LINE_H             = %d\r\n", LINE_H);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: CHARS_PER_LINE     = %d\r\n", CHARS_PER_LINE);
    UART_PRINT(buf);

    snprintf(buf, sizeof(buf), "DIAG: CONTENT_MAX_LINES  = %d\r\n", CONTENT_MAX_LINES);
    UART_PRINT(buf);

    // ------------------------------------------------------------------
    // 2. Clear to black and draw calibration pattern
    // ------------------------------------------------------------------
    fillScreen(BLACK);

    // Blue rectangle for banner zone (rows 0..BANNER_H-1)
    fillRect(0u, 0u, (unsigned int)SCREEN_W,
             (unsigned int)BANNER_H, BLUE);

    // Full-screen red border — 1px thick on all four edges
    // Top edge
    drawFastHLine(0, 0,            SCREEN_W,     RED);
    // Bottom edge
    drawFastHLine(0, SCREEN_H - 1, SCREEN_W,     RED);
    // Left edge
    drawFastVLine(0,           0,  SCREEN_H,     RED);
    // Right edge
    drawFastVLine(SCREEN_W - 1, 0, SCREEN_H,     RED);

    // Green cross-hairs at display centre
    drawFastHLine(0,           SCREEN_H / 2, SCREEN_W, GREEN);
    drawFastVLine(SCREEN_W / 2, 0,           SCREEN_H, GREEN);

    // Cyan line at CONTENT_Y (banner/content boundary)
    drawFastHLine(0, CONTENT_Y, SCREEN_W, CYAN);

    // White pixel at each of the four corners
    drawPixel(0,            0,            WHITE);
    drawPixel(SCREEN_W - 1, 0,            WHITE);
    drawPixel(0,            SCREEN_H - 1, WHITE);
    drawPixel(SCREEN_W - 1, SCREEN_H - 1, WHITE);

    // Magenta dot every 10 rows along the left edge (x=1 to stay visible)
    for (i = 0; i < SCREEN_H; i += 10)
        drawPixel(1, i, MAGENTA);

    // Yellow dot every 10 columns along the top edge
    for (i = 0; i < SCREEN_W; i += 10)
        drawPixel(i, 1, YELLOW);

    // ------------------------------------------------------------------
    // 3. Text labels at known pixel positions
    //    If these appear in the wrong place you know the coord mapping.
    // ------------------------------------------------------------------
    // "0,0" near top-left (x=2, y=2) — inside banner zone, expect blue bg
    ui_str(2, 2, "0,0", WHITE, BLUE, 1);

    // Bottom-right label — drawn at (SCREEN_W - 6*7, SCREEN_H - CHAR_H - 1)
    //   = (128 - 42, 128 - 9) = (86, 119) for a 7-char string
    ui_str(SCREEN_W - CHAR_W * 7, SCREEN_H - CHAR_H - 1,
           "127,127", YELLOW, BLACK, 1);

    // Mid-screen label at the centre cross-hair
    ui_str(SCREEN_W / 2 - CHAR_W * 3, SCREEN_H / 2 + 2,
           "64,64", GREEN, BLACK, 1);

    // Content-Y label
    snprintf(buf, sizeof(buf), "Y=%d", CONTENT_Y);
    ui_str(SCREEN_W - CHAR_W * (int)strlen(buf), CONTENT_Y + 2,
           buf, CYAN, BLACK, 1);

    // ------------------------------------------------------------------
    // 4. Done
    // ------------------------------------------------------------------
    UART_PRINT("DIAG: visual pattern drawn -- check OLED for:\r\n");
    UART_PRINT("      red border at all 4 edges (0,0)..(127,127)\r\n");
    UART_PRINT("      green crosshairs at centre (64,64)\r\n");
    UART_PRINT("      blue banner zone rows 0-11\r\n");
    UART_PRINT("      cyan line at content boundary\r\n");
    UART_PRINT("      magenta dots left edge every 10 rows\r\n");
    UART_PRINT("      yellow dots top edge every 10 cols\r\n");
    UART_PRINT("===== END DIAGNOSTICS =====\r\n\r\n");
}
