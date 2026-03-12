// LRCLIB Lyrics API Client
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// ===========================================================================
// OVERVIEW
// ===========================================================================
// This module queries the LRCLIB public REST API over the CC3200 Wi-Fi
// connection and populates the OLED-UI lyrics view with song lyrics.
//
// LRCLIB (https://lrclib.net) is a free, community-maintained lyrics database
// with no API key required.  It provides both plain (static) lyrics and
// synced (timestamped, LRC-format) lyrics.
//
// All requests go to:
//   https://lrclib.net/api/get  (HTTPS/TLS, port 443)
//
// Example request path:
//   /api/get?artist_name=Tame+Impala&track_name=Let+It+Happen
//
// SYNCED LYRICS
// -------------
// If the response includes a "syncedLyrics" field, its value is an LRC-format
// string:  "[MM:SS.xx]Line text\n[MM:SS.xx]Line text\n..."
//
// Call LRCLib_FetchLyrics() once when a new song is detected.  Then call
// LRCLib_UpdateSyncedDisplay(elapsed_ms) periodically from the main loop
// (e.g. every 500 ms) to advance the lyrics view to the current line.
// Follow each call that returns 1 with oled_ui_render() to push the change.
//
// ===========================================================================
// NOTES ON TLS
// ===========================================================================
// The CC3200 SimpleLink TLS stack does not ship with root CA certificates
// pre-loaded, so server certificate verification is skipped (no call to
// SL_SO_SECURE_FILES_CA_FILE_NAME).  The TLS handshake still encrypts the
// transport; only certificate chain validation is absent.

#ifndef LRCLIB_H_
#define LRCLIB_H_

#include <stdbool.h>

// ---------------------------------------------------------------------------
// Network constants
// ---------------------------------------------------------------------------

// LRCLIB API hostname (HTTPS only)
#define LRCLIB_HOST              "lrclib.net"
#define LRCLIB_PORT              443

// HTTP receive buffer.  LRCLIB JSON responses (plain + synced lyrics combined)
// are typically 3-8 KB for pop/rock songs.
#define LRCLIB_HTTP_BUF_SIZE     8192

// Socket receive timeout (milliseconds)
#define LRCLIB_RECV_TIMEOUT_MS   8000

// ---------------------------------------------------------------------------
// Synced lyrics storage limits
// ---------------------------------------------------------------------------

// Maximum number of timestamped lyric lines stored in RAM.
// 80 lines covers most songs (7-8 minute pop/rock tracks typically have
// 40-70 couplets).  At 84 bytes per line this costs about 6.7 KB of RAM.
#define LRCLIB_MAX_LYRIC_LINES   80

// Maximum characters per synced lyric line (not including NUL terminator)
#define LRCLIB_MAX_LINE_LEN      80

// ---------------------------------------------------------------------------
// Return codes
// ---------------------------------------------------------------------------
#define LRCLIB_OK                0    // success
#define LRCLIB_ERR_NOT_INIT     -1    // LRCLib_Init() not called
#define LRCLIB_ERR_DNS          -2    // hostname resolution failed
#define LRCLIB_ERR_SOCKET       -3    // socket create / connect failed
#define LRCLIB_ERR_SEND         -4    // HTTP request send failed
#define LRCLIB_ERR_RECV         -5    // HTTP response receive failed / timeout
#define LRCLIB_ERR_HTTP         -6    // Unexpected HTTP status code
#define LRCLIB_ERR_PARSE        -7    // Required JSON field not found
#define LRCLIB_ERR_NOT_FOUND    -8    // HTTP 404: track not in LRCLIB database

// ***************************************************************************
// Public API
// ***************************************************************************

/**
 * LRCLib_Init
 *
 * Resets internal state and marks the module as ready.
 * Must be called once after the Wi-Fi connection is established
 * (after connectToAccessPoint() returns successfully).
 * No network access is performed.
 */
void LRCLib_Init(void);

/**
 * LRCLib_FetchLyrics
 *
 * Queries the LRCLIB /api/get endpoint for the given artist and track.
 *
 * On success:
 *   - oled_ui_update_lyrics(true, plain_text) is called immediately so the
 *     lyrics view can render static lyrics right away.
 *   - If the response contains a "syncedLyrics" field, the timestamped lines
 *     are parsed and stored internally; LRCLib_HasSyncedLyrics() returns true.
 *   - If the track is marked "instrumental":true, the lyrics view is updated
 *     with the string "(Instrumental)" and LRCLIB_OK is returned.
 *
 * On failure:
 *   - oled_ui_update_lyrics(false, NULL) is called so the view shows its
 *     default "unavailable" placeholder.
 *   - The error code is stored and retrievable via LRCLib_GetLastError().
 *
 * URL-encoding of artist/track is handled internally.
 *
 * @param artist  Artist name, e.g. "Tame Impala"
 * @param track   Track title, e.g. "Let It Happen"
 *
 * @return  LRCLIB_OK on success, or a negative LRCLIB_ERR_* code.
 */
int LRCLib_FetchLyrics(const char *artist, const char *track);

/**
 * LRCLib_UpdateSyncedDisplay
 *
 * Advances the OLED lyrics view to the lyric line that corresponds to
 * elapsed_ms.  Builds a display window (current line through end of song)
 * and calls oled_ui_update_lyrics() + oled_ui_reset_scroll() only when the
 * active line changes, minimising unnecessary OLED writes.
 *
 * Has no effect if no synced lyrics have been fetched (safe to call even
 * when only plain lyrics are available).
 *
 * Typical usage in main loop:
 *   if (LRCLib_HasSyncedLyrics()) {
 *       if (LRCLib_UpdateSyncedDisplay(elapsed_ms))
 *           oled_ui_render();
 *   }
 *
 * @param elapsed_ms  Milliseconds elapsed since the song started playing.
 *                    Obtain from a CC3200 timer or by tracking wall-clock
 *                    ticks since the song was first detected via RDS.
 *
 * @return  1 if the current lyric line changed (caller should call
 *            oled_ui_render() if the lyrics view is active).
 *          0 if the current line is unchanged (no redraw needed).
 */
int LRCLib_UpdateSyncedDisplay(unsigned long elapsed_ms);

/**
 * LRCLib_HasSyncedLyrics
 *
 * @return  true  if parsed timestamped lyric lines are available from the
 *                most recent successful call to LRCLib_FetchLyrics().
 *          false otherwise (not initialised, fetch failed, or plain-only).
 */
bool LRCLib_HasSyncedLyrics(void);

/**
 * LRCLib_GetLastError
 *
 * Returns the LRCLIB_ERR_* code from the most recent operation that failed,
 * or LRCLIB_OK if the last operation succeeded.  Useful for UART diagnostics.
 */
int LRCLib_GetLastError(void);

#endif /* LRCLIB_H_ */
