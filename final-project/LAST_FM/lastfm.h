// LastFM API Client
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// ===========================================================================
// OVERVIEW
// ===========================================================================
// This module queries the Last.fm public REST API over the CC3200 Wi-Fi
// connection and populates all OLED-UI views with data for the
// currently playing song and artist.
//
// All requests go to:
//   http://ws.audioscrobbler.com/2.0/   (plain HTTP, port 80)
//
// Album cover art is fetched as a JPEG from the Last.fm CDN and decoded
// line-by-line using TJpgDec (Tiny JPEG Decompressor). See JPEG SETUP below.
//
// ===========================================================================
// JPEG SETUP  (required for OLED_VIEW_ALBUM_COVER)
// ===========================================================================
//     JD_SZBUF   512     -- input chunk size (bytes); keep small to save RAM
//     JD_FORMAT  0       -- output format 0 = RGB888 (3 bytes/pixel)
//     JD_USE_SCALE  1    -- enable 1/2 / 1/4 / 1/8 scaling
//     JD_TBLCLIP    1    -- fast clipping table (saves ~500 B RAM vs math)

#ifndef LASTFM_H_
#define LASTFM_H_

#include <stdbool.h>
#include <common.h>


// Last.fm JSON API endpoint (plain HTTP, no TLS needed for API calls)
#define LASTFM_API_HOST          "ws.audioscrobbler.com"
#define LASTFM_API_PORT          80

// Last.fm CDN host for album art images (requires TLS — port 443)
#define LASTFM_CDN_HOST          "lastfm.freetls.fastly.net"
#define LASTFM_CDN_PORT          443

// HTTP receive buffer.  Last.fm JSON responses are typically 2–8 KB.
// CC3200 has 256 KB RAM total; 6 KB here is a reasonable balance.
#define LASTFM_HTTP_BUF_SIZE     6144

// Socket receive timeout (milliseconds).
// Increase on slow Wi-Fi; decrease to fail faster on dead stations.
#define LASTFM_RECV_TIMEOUT_MS   8000

// Maximum number of similar artists / tracks / tags to extract.
// Must be <= UI_MAX_LIST_ITEMS (10).
#define LASTFM_MAX_LIST_ITEMS    5

// Maximum image URL length stored internally.
#define LASTFM_MAX_IMG_URL_LEN   128

// ---------------------------------------------------------------------------
// Return codes
// ---------------------------------------------------------------------------
#define LASTFM_OK                 0   // success
#define LASTFM_ERR_NOT_INIT      -1   // LastFM_Init() not called
#define LASTFM_ERR_DNS           -2   // hostname resolution failed
#define LASTFM_ERR_SOCKET        -3   // could not create / connect socket
#define LASTFM_ERR_SEND          -4   // HTTP request send failed
#define LASTFM_ERR_RECV          -5   // HTTP response receive failed / timeout
#define LASTFM_ERR_HTTP          -6   // HTTP status code != 200
#define LASTFM_ERR_PARSE         -7   // JSON field not found in response
#define LASTFM_ERR_JPEG          -8   // TJpgDec decode error (JPEG support)

// ***************************************************************************
// Public API
// ***************************************************************************

/**
 * LastFM_Init
 *
 * Stores the Last.fm API key and resets internal state.
 * Must be called once after the Wi-Fi connection is established
 * (after connectToAccessPoint() returns successfully).
 *
 * @param api_key   NUL-terminated Last.fm API key string (32 hex chars).
 *                  Register at https://www.last.fm/api/account/create
 */
void LastFM_Init(const char *api_key);

/**
 * LastFM_QueryAndUpdateViews
 *
 * Primary entry point.  Makes up to three sequential HTTP GET requests to
 * Last.fm and updates the OLED-UI data stores for all text-based views:
 *
 *   OLED_VIEW_GENRE_TAGS
 *   OLED_VIEW_ARTIST_BIO
 *   OLED_VIEW_SIMILAR_ARTISTS
 *   OLED_VIEW_SIMILAR_TRACKS  (see known limitations in function header)
 *
 *   OLED_VIEW_LYRICS
 *   OLED_VIEW_ALBUM_COVER     image URL cached; art NOT drawn yet
 *                                 (call LastFM_RenderAlbumCover() separately)
 *
 * Call oled_ui_render() afterwards to push changes to the screen.
 *
 * @param artist   Artist name, e.g. "Daft Punk"  (URL-encoding is internal)
 * @param track    Track title, e.g. "Get Lucky"
 *
 * @return  Number of successful API calls (0..3).  A partial result
 *          (e.g. 2) means some views were updated while others failed.
 *          LASTFM_ERR_NOT_INIT if LastFM_Init() was never called.
 */
int LastFM_QueryAndUpdateViews(const char *artist, const char *track);

/**
 * LastFM_RenderAlbumCover
 *
 * Fetches the cached album-art JPEG from the Last.fm CDN over a TLS socket
 * and decodes it directly onto the OLED using TJpgDec.  Each MCU block is
 * drawn via drawPixel() as it is decoded (no full-frame buffer needed).
 *
 * The image is scaled to fill the OLED content area (128 x 116 px) with
 * nearest-neighbour interpolation while preserving the aspect ratio
 * (unused margins are filled with black).
 *
 * Prerequisites:
 *   - LastFM_QueryAndUpdateViews() has been called and succeeded for at
 *     least track.getInfo (so that the album art URL is cached).
 *   - LASTFM_ENABLE_JPEG must be defined and TJpgDec sources present.
 *   - The CC3200 Wi-Fi connection must still be active.
 *
 * If album art is unavailable or JPEG support is not compiled in, a grey
 * placeholder rectangle with the text "No Art" is drawn instead.
 *
 * This function takes approximately 1–3 seconds on typical Wi-Fi.
 * Calling it while a different OLED view is active will corrupt that view's
 * content; navigate to OLED_VIEW_ALBUM_COVER first.
 *
 * @return  LASTFM_OK on success, or a negative error code.
 */
int LastFM_RenderAlbumCover(void);

/**
 * LastFM_AlbumArtAvailable
 *
 * @return  true  if a valid album art URL was cached by the last call to
 *                LastFM_QueryAndUpdateViews().
 *          false if no URL is available (no track queried, or track.getInfo
 *                returned no image).
 */
bool LastFM_AlbumArtAvailable(void);

/**
 * LastFM_GetLastError
 *
 * Returns the LASTFM_ERR_* code from the most recent API call that failed,
 * or LASTFM_OK if the last operation succeeded.  Useful for UART diagnostics.
 */
int LastFM_GetLastError(void);

#endif
