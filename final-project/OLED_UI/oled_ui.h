// Graphical UI OLED API
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// OVERVIEW
// --------
// This module owns the entire 128x128 OLED surface and renders one of seven
// "views" at a time.  A thin navigation banner sits at the top; all remaining
// pixels belong to the active view's content area.
//
// TYPICAL CALL SEQUENCE
// ---------------------
//   1. oled_ui_init()                    — call once after Adafruit_Init()
//   2. oled_ui_update_radio(...)         — push fresh data (does NOT redraw)
//   3. oled_ui_render()                  — repaint the screen
//   On a remote button:
//     oled_ui_navigate_left/right()      — switch view  (does NOT redraw)
//     oled_ui_scroll_up/down()           — scroll content (does NOT redraw)
//     oled_ui_render()                   — repaint
//
// EXTENDING WITH NEW VIEWS
// ------------------------
//   1. Add a new entry to OledViewID (before OLED_VIEW_COUNT).
//   2. Add a 3-char label to banner_labels[] in oled_ui.c.
//   3. Add a data struct and update function if needed.
//   4. Write a static render_*() function in oled_ui.c.
//   5. Add the case to the switch in oled_ui_render().

#ifndef OLED_UI_H
#define OLED_UI_H

#include <stdbool.h>

// ***************************************************************************
// View identifiers
// ***************************************************************************
typedef enum {
    OLED_VIEW_RADIO            = 0,
    OLED_VIEW_ALBUM_COVER      = 1,
    OLED_VIEW_SIMILAR_ARTISTS  = 2,
    OLED_VIEW_ARTIST_BIO       = 3,
    OLED_VIEW_GENRE_TAGS       = 4,
    OLED_VIEW_SIMILAR_TRACKS   = 5,
    OLED_VIEW_LYRICS           = 6,
    OLED_VIEW_COUNT            = 7   // *** Always keep last ***
} OledViewID;


// ***************************************************************************
// String / list capacity limits
// Tune these to balance feature completeness vs. CC3200 RAM usage.
// ***************************************************************************
#define UI_MAX_STATION_LEN      16   // e.g. "90.3 FM"
#define UI_MAX_CALLSIGN_LEN     16   // e.g. "KDVS"
#define UI_MAX_SONG_LEN         64
#define UI_MAX_ARTIST_LEN       64
#define UI_MAX_LIST_ITEMS       10   // max entries in any list view
#define UI_MAX_LIST_ITEM_LEN    64   // max chars per list entry
#define UI_MAX_BIO_LEN         1024  // artist biography
#define UI_MAX_LYRICS_LEN      2048  // song lyrics


// ***************************************************************************
// VIEW data structures
// Populate these via the oled_ui_update_*() functions, then call
// oled_ui_render() to push changes to the screen.
// ***************************************************************************

// View 0 — Radio
typedef struct {
    char station[UI_MAX_STATION_LEN];    // "90.3 FM"
    char callsign[UI_MAX_CALLSIGN_LEN];  // "KDVS"
    char song[UI_MAX_SONG_LEN];          // Current song title (from RDS)
    char artist[UI_MAX_ARTIST_LEN];      // Current artist   (from RDS)
    int  progress_pct;                   // Track progress 0-100
    int  signal_strength;                // RSSI 0-100
} RadioViewData;

// View 1 — Album Cover
// Full JPEG rendering is deferred pending TJpgDec integration.
// Set available=true and supply data when ready; the stub will be replaced.
typedef struct {
    bool available;
    // Future fields (TJpgDec):
    //   const unsigned char *jpeg_data;
    //   unsigned int         jpeg_len;
} AlbumCoverData;

// Views 2, 4, 5 — Generic ordered list (similar artists / genre tags / tracks)
typedef struct {
    char items[UI_MAX_LIST_ITEMS][UI_MAX_LIST_ITEM_LEN];
    int  count;
} ListViewData;

// View 3 — Artist Biography
typedef struct {
    char text[UI_MAX_BIO_LEN];
} ArtistBioData;

// View 6 — Song Lyrics
typedef struct {
    bool available;
    char text[UI_MAX_LYRICS_LEN];
} LyricsData;

// ***************************************************************************
// Public API
// ***************************************************************************

// Initialize the UI, zero all data, and render the default (radio) view.
// Call once after Adafruit_Init() and SPIconfig().
void oled_ui_init(void);

// Cycle to the previous / next view (wraps around).
// Resets the scroll position of the new view to the top.
// Does NOT redraw; call oled_ui_render() afterwards.
void oled_ui_navigate_left(void);
void oled_ui_navigate_right(void);

// Jump directly to a named view (resets scroll).
// Does NOT redraw; call oled_ui_render() afterwards.
void oled_ui_set_view(OledViewID view);

// Returns the currently displayed view.
OledViewID oled_ui_get_view(void);

// Move up/down one unit within the current view's content.
//   - List views:  one unit = one list item.
//   - Text views:  one unit = one wrapped line (6px).
// Scroll is clamped during render; calling scroll_down past the end is safe.
// Does NOT redraw; call oled_ui_render() afterwards.
void oled_ui_scroll_up(void);
void oled_ui_scroll_down(void);

// Return the active view's scroll position to the top.
// Does NOT redraw; call oled_ui_render() afterwards.
void oled_ui_reset_scroll(void);

// Repaint the banner and the current view using the latest stored data.
// This is the only function that writes to the OLED hardware.
void oled_ui_render(void);

// ---- Data update functions ----
// None of these functions trigger a redraw.  Call oled_ui_render() when
// you want the new data to appear on screen.

// View 0 — Radio
// Pass NULL for any string you do not wish to update.
void oled_ui_update_radio(const char *station,
                           const char *callsign,
                           const char *song,
                           const char *artist,
                           int progress_pct,
                           int signal_strength);

// View 1 — Album Cover
// Set available=false while art is loading or unavailable.
void oled_ui_update_album_cover(bool available);
// Placeholder for future JPEG path:
// void oled_ui_update_album_cover_jpeg(const unsigned char *data, unsigned int len);

// View 2 — Similar Artists
// artists[][UI_MAX_LIST_ITEM_LEN], count <= UI_MAX_LIST_ITEMS
void oled_ui_update_similar_artists(const char artists[][UI_MAX_LIST_ITEM_LEN],
                                     int count);

// View 3 — Artist Biography
void oled_ui_update_artist_bio(const char *bio);

// View 4 — Genre Tags
void oled_ui_update_genre_tags(const char tags[][UI_MAX_LIST_ITEM_LEN],
                                int count);

// View 5 — Similar Tracks
void oled_ui_update_similar_tracks(const char tracks[][UI_MAX_LIST_ITEM_LEN],
                                    int count);

// View 6 — Song Lyrics
// Set available=false (and lyrics=NULL) when lyrics cannot be retrieved.
void oled_ui_update_lyrics(bool available, const char *lyrics);



// Draws a corner-to-corner calibration pattern on the OLED and prints
// all relevant dimension constants over UART0 (uart_if Message/UART_PRINT).
// Call instead of oled_ui_render() to troubleshoot layout issues.
void oled_ui_draw_diagnostics(void);


#endif
