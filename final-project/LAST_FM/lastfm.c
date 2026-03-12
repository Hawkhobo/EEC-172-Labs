// LastFM API Client Implementation
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// See lastfm.h for full documentation.
//
// SEPARATION OF RESPONSIBILITIES
// --------------------------------
//   lastfm.c  -- network I/O only: DNS, sockets, HTTP/TLS, JSON parsing,
//                and jpeg_in_cb (streams bytes from the socket into TJpgDec)
//   oled_ui.c -- all pixel drawing: jpeg_out_cb, scale computation,
//                TJpgDec work buffer, jd_prepare/jd_decomp call site

// ===========================================================================
// 1  Includes & compile-time guards
// ===========================================================================

#include <LAST_FM/lastfm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// SimpleLink / network stack
#include "simplelink.h"

// UART debug
#include "uart_if.h"

// OLED UI data update API  (to populate views)
#include "../OLED_UI/oled_ui.h"

// LRCLIB API to fetch song lyrics
#include "../LRCLIB/lrclib.h"

// TJpgDec support see lastfm.h JPEG SETUP
#include "tjpgdec/tjpgd.h"

// ===========================================================================
// 2  Internal constants & macros
// ===========================================================================

// Last.fm JSON API base path template (used by query_* helpers)
#define _API_PATH_BASE   "/2.0/?method="

// ===========================================================================
// 3  Module state
// ===========================================================================

static bool   s_init            = false;
static char   s_api_key[48]     = {0};
static char   s_img_url[LASTFM_MAX_IMG_URL_LEN] = {0};  // cached art URL
static int    s_last_error      = LASTFM_OK;

// Shared HTTP receive buffer reused for every HTTP transaction.
// All callers are sequential; no concurrency issues.
static char   s_http_buf[LASTFM_HTTP_BUF_SIZE];

// ===========================================================================
// 4  Internal utility helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// 4a  URL encoding
//
// Encodes src into dst (max dst_len bytes including NUL).
// Spaces become '+'; other non-unreserved chars become %XX.
// ---------------------------------------------------------------------------
static void url_encode(const char *src, char *dst, int dst_len)
{
    static const char *k_hex = "0123456789ABCDEF";
    int out = 0;

    while (*src && out < dst_len - 4) {
        unsigned char c = (unsigned char)*src++;

        if (c == ' ') {
            dst[out++] = '+';
        } else if ((c >= 'A' && c <= 'Z') ||
                   (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '%';
            dst[out++] = k_hex[(c >> 4) & 0x0F];
            dst[out++] = k_hex[ c       & 0x0F];
        }
    }
    dst[out] = '\0';
}

// ---------------------------------------------------------------------------
// 4b  JSON string extraction
//
// Searches json for the first occurrence of  "key":"VALUE"  and copies
// VALUE (with JSON escape processing) into out[0..max_len-1].
//
// Returns 1 on success, 0 if the key was not found or the value is not
// a JSON string.  Handles \n \t \" \\ \/ \uXXXX (basic BMP only).
// ---------------------------------------------------------------------------
static int json_get_string(const char *json,
                            const char *key,
                            char       *out,
                            int         max_len)
{
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);

    // Skip whitespace between ':' and the opening '"'
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return 0;
    p++;  // consume opening quote

    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case '/':  out[i++] = '/';  break;
                case 'r':  out[i++] = '\r'; break;
                case 'u': {
                    // \uXXXX use '?' for non-ASCII (OLED font is ASCII only)
                    if (*(p+1) && *(p+2) && *(p+3) && *(p+4)) {
                        unsigned int cp = 0;
                        sscanf(p + 1, "%4x", &cp);
                        out[i++] = (cp < 128) ? (char)cp : '?';
                        p += 4;
                    }
                    break;
                }
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

// ---------------------------------------------------------------------------
// 4c  JSON name-array extraction
//
// Finds the JSON array keyed by arr_key within the string starting at
// json_start, then extracts the "name" string from each object element.
//
// Example:  arr_key = "tag"  for  "tag":[{"name":"pop","url":"..."},...]
//
// Returns the number of names placed into names[][].  At most max_items
// entries are returned; each is NUL-terminated and up to item_max_len chars.
// ---------------------------------------------------------------------------
static int json_get_name_array(const char *json_start,
                                const char *arr_key,
                                char        names[][UI_MAX_LIST_ITEM_LEN],
                                int         max_items)
{
    // Build the array search key, e.g.  "tag":[
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":[", arr_key);

    const char *p = strstr(json_start, search);
    if (!p) return 0;
    p += strlen(search);   // p now points just past the opening '['

    int count = 0;
    char obj_buf[256];     // temporary buffer for one JSON object

    while (count < max_items && *p) {
        // Skip whitespace / commas between objects
        while (*p == ' ' || *p == '\t' || *p == '\r' ||
               *p == '\n' || *p == ',') p++;

        if (*p == ']') break;  // end of array
        if (*p != '{') { p++; continue; }

        // Find the matching closing '}'  (track nesting depth)
        const char *obj_start = p;
        int depth = 0;
        const char *q = p;
        while (*q) {
            if      (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }

        // Copy object to stack buffer (truncate to fit)
        int obj_len = (int)(q - obj_start);
        if (obj_len >= (int)sizeof(obj_buf))
            obj_len = (int)sizeof(obj_buf) - 1;
        memcpy(obj_buf, obj_start, obj_len);
        obj_buf[obj_len] = '\0';

        // Extract "name" from this object
        if (json_get_string(obj_buf, "name", names[count],
                            UI_MAX_LIST_ITEM_LEN)) {
            count++;
        }
        p = q;
    }
    return count;
}

// ---------------------------------------------------------------------------
// 4d  HTML / entity stripping
//
// Strips <tag> sequences from str in-place and converts the most common
// HTML entities to their ASCII equivalents.  Replaces stripped tags with
// a single space to avoid words running together.
// ---------------------------------------------------------------------------
static void strip_html(char *str)
{
    char *src = str;
    char *dst = str;
    bool in_tag = false;

    while (*src) {
        if (*src == '<') {
            in_tag = true;
            // Emit one space so words don't merge across stripped tags
            if (dst > str && *(dst - 1) != ' ') *dst++ = ' ';
        } else if (*src == '>' && in_tag) {
            in_tag = false;
        } else if (!in_tag) {
            // Basic HTML entity decoding
            if (*src == '&') {
                if      (strncmp(src, "&amp;",  5) == 0) { *dst++ = '&';  src += 4; }
                else if (strncmp(src, "&lt;",   4) == 0) { *dst++ = '<';  src += 3; }
                else if (strncmp(src, "&gt;",   4) == 0) { *dst++ = '>';  src += 3; }
                else if (strncmp(src, "&quot;", 6) == 0) { *dst++ = '"';  src += 5; }
                else if (strncmp(src, "&#39;",  5) == 0) { *dst++ = '\''; src += 4; }
                else if (strncmp(src, "&nbsp;", 6) == 0) { *dst++ = ' ';  src += 5; }
                else { *dst++ = *src; }
            } else {
                *dst++ = *src;
            }
        }
        src++;
    }
    *dst = '\0';
}

// ---------------------------------------------------------------------------
// Helper: collapse consecutive whitespace in str in-place.
// Replaces any run of \r, \n, \t, or multiple spaces with a single space.
// ---------------------------------------------------------------------------
static void collapse_whitespace(char *str)
{
    char *src = str;
    char *dst = str;
    bool last_space = false;

    while (*src) {
        if (*src == '\r' || *src == '\n' || *src == '\t' || *src == ' ') {
            if (!last_space && dst > str) {
                *dst++ = ' ';
                last_space = true;
            }
        } else {
            *dst++ = *src;
            last_space = false;
        }
        src++;
    }
    // Trim trailing space
    if (dst > str && *(dst - 1) == ' ') dst--;
    *dst = '\0';
}

// ---------------------------------------------------------------------------
// Helper: find the start of the HTTP body (skip headers).
// Returns pointer to first byte after "\r\n\r\n", or NULL if not found.
// Also extracts the HTTP status code into *status_code.
// ---------------------------------------------------------------------------
static const char *http_skip_headers(const char *resp, int resp_len,
                                      int *status_code)
{
    // Parse status line:  "HTTP/1.x NNN ..."
    if (status_code) {
        *status_code = 0;
        if (strncmp(resp, "HTTP/", 5) == 0) {
            const char *sp = strchr(resp, ' ');
            if (sp) *status_code = atoi(sp + 1);
        }
    }

    // Find end of headers
    const char *body = NULL;
    const char *p = resp;
    int remaining = resp_len;

    while (remaining >= 4) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            body = p + 4;
            break;
        }
        p++;
        remaining--;
    }
    return body;
}

// ===========================================================================
// 5a  Low-level plain HTTP GET  (port 80 used for all JSON calls)
//
// Resolves hostname, opens TCP socket, sends a minimal HTTP/1.0 GET
// request, and receives the full response into s_http_buf.
//
// Returns the number of bytes in s_http_buf on success (includes headers),
// or a negative LASTFM_ERR_* on failure.
// ===========================================================================
static int http_get_plain(const char *host, int port, const char *path)
{
    // --- DNS resolve ---
    unsigned long ulIP = 0;
    long rc = sl_NetAppDnsGetHostByName(
                  (signed char *)host, (unsigned short)strlen(host),
                  &ulIP, SL_AF_INET);
    if (rc < 0) {
        UART_PRINT("[LastFM] DNS failed for %s (rc=%ld)\n\r", host, rc);
        return LASTFM_ERR_DNS;
    }

    // --- Create socket ---
    int sock = sl_Socket(SL_AF_INET, SL_SOCK_STREAM, SL_IPPROTO_TCP);
    if (sock < 0) {
        UART_PRINT("[LastFM] Socket create failed (%d)\n\r", sock);
        return LASTFM_ERR_SOCKET;
    }

    // Set receive timeout
    SlTimeval_t tv;
    tv.tv_sec  = LASTFM_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (LASTFM_RECV_TIMEOUT_MS % 1000) * 1000;
    sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof(tv));

    // --- Connect ---
    SlSockAddrIn_t addr;
    addr.sin_family      = SL_AF_INET;
    addr.sin_port        = sl_Htons((unsigned short)port);
    addr.sin_addr.s_addr = sl_Htonl(ulIP);

    rc = sl_Connect(sock, (SlSockAddr_t *)&addr, sizeof(addr));
    if (rc < 0) {
        UART_PRINT("[LastFM] Connect failed (%ld)\n\r", rc);
        sl_Close(sock);
        return LASTFM_ERR_SOCKET;
    }

    // --- Build and send HTTP/1.0 GET request ---
    // HTTP/1.0 closes the connection after the response no chunked encoding,
    // no keep-alive complexity.
    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: CC3200-FM-Explorer/1.0\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    if (sl_Send(sock, req, req_len, 0) < 0) {
        UART_PRINT("[LastFM] Send failed\n\r");
        sl_Close(sock);
        return LASTFM_ERR_SEND;
    }

    // --- Receive response into s_http_buf ---
    int total = 0;
    int space = LASTFM_HTTP_BUF_SIZE - 1;

    while (space > 0) {
        int n = sl_Recv(sock, s_http_buf + total, space, 0);
        if (n < 0) {
            // Timeout or error treat partial data as the full response
            // if we already received something
            if (total == 0) {
                UART_PRINT("[LastFM] Recv failed (%d)\n\r", n);
                sl_Close(sock);
                return LASTFM_ERR_RECV;
            }
            break;
        }
        if (n == 0) break;  // connection closed by server (normal for HTTP/1.0)
        total += n;
        space -= n;
    }

    sl_Close(sock);
    s_http_buf[total] = '\0';

    UART_PRINT("[LastFM] HTTP GET %s -> %d bytes\n\r", path, total);
    return total;
}

// ===========================================================================
// 6  Last.fm API call implementations
// ===========================================================================

// ---------------------------------------------------------------------------
// 6a  track.getInfo
//
// Fetches track metadata and fills:
//   - s_img_url        (cached album-art URL, "extralarge" preferred)
//   - OLED genre tags  (up to LASTFM_MAX_LIST_ITEMS tags)
//   - Corrected artist / track names via autocorrect
//
// Returns LASTFM_OK on success, LASTFM_ERR_* on failure.
// ---------------------------------------------------------------------------
static int query_track_info(const char *artist, const char *track,
                             char *out_corrected_artist,
                             char *out_corrected_track,
                             int   name_max)
{
    char enc_artist[128], enc_track[128];
    url_encode(artist, enc_artist, sizeof(enc_artist));
    url_encode(track,  enc_track,  sizeof(enc_track));

    char path[512];
    // Build the full request path directly
    snprintf(path, sizeof(path),
             "/2.0/?method=track.getinfo"
             "&artist=%s&track=%s"
             "&api_key=%s"
             "&autocorrect=1"
             "&format=json",
             enc_artist, enc_track, s_api_key);

    int bytes = http_get_plain(LASTFM_API_HOST, LASTFM_API_PORT, path);
    if (bytes < 0) return bytes;

    int status = 0;
    const char *body = http_skip_headers(s_http_buf, bytes, &status);
    if (!body || status != 200) return LASTFM_ERR_HTTP;

    // --- Extract corrected names ---
    // The "track" object contains "name" (corrected track title)
    // and nested "artist" object contains "name" (corrected artist name).
    // We extract using scoped searches to avoid collisions with similar keys.
    if (out_corrected_track) {
        json_get_string(body, "name", out_corrected_track, name_max);
    }
    if (out_corrected_artist) {
        // Find the artist object nested inside the track object
        const char *artist_obj = strstr(body, "\"artist\":{");
        if (artist_obj) {
            // Move past the outer "artist":{ so we're inside the object
            const char *inner = artist_obj + strlen("\"artist\":{");
            json_get_string(inner, "name", out_corrected_artist, name_max);
        }
    }

    // --- Extract album art URL (prefer "extralarge", fall back to "large") ---
    // Image array structure:  "image":[{"#text":"URL","size":"small"},...,"extralarge"}]
    // Strategy: find the last "extralarge" size entry or the last "large" entry.
    s_img_url[0] = '\0';

    // Look for the album block first (avoid picking up artist images)
    const char *album_start = strstr(body, "\"album\":{");
    const char *search_region = album_start ? album_start : body;

    // Look for the album block first (avoid picking up artist/track-level images)
   {
       const char *sp = search_region;
       char best_large_url[LASTFM_MAX_IMG_URL_LEN];
       char candidate_url[LASTFM_MAX_IMG_URL_LEN];
       char size_val[32];
       const char *end_of_album;

       best_large_url[0] = '\0';

       /* Limit search to the album block if we found one */
       end_of_album = NULL;
       if (album_start) {
           /* Find the closing brace of the "album":{...} object */
           int depth = 0;
           const char *q = album_start + strlen("\"album\":");
           while (*q) {
               if      (*q == '{') depth++;
               else if (*q == '}') { depth--; if (depth == 0) { end_of_album = q + 1; break; } }
               q++;
           }
       }

       while ((sp = strstr(sp, "\"#text\":")) != NULL) {
           /* Stop if we've passed the end of the album block */
           if (end_of_album && sp >= end_of_album) break;

           candidate_url[0] = '\0';
           if (!json_get_string(sp, "#text", candidate_url, sizeof(candidate_url))) {
               sp++;
               continue;
           }

           size_val[0] = '\0';
           json_get_string(sp + 8, "size", size_val, sizeof(size_val));

           if (strlen(candidate_url) > 4) {
               // prefer "large" over "extralarge", better for the small OLED
               if (strcmp(size_val, "large") == 0) {
                   /* Best option take it immediately */
                   strncpy(s_img_url, candidate_url, LASTFM_MAX_IMG_URL_LEN - 1);
                   s_img_url[LASTFM_MAX_IMG_URL_LEN - 1] = '\0';
                   break;
               } else if (strcmp(size_val, "extralarge") == 0 && best_large_url[0] == '\0') {
                   strncpy(best_large_url, candidate_url, LASTFM_MAX_IMG_URL_LEN - 1);
                   best_large_url[LASTFM_MAX_IMG_URL_LEN - 1] = '\0';
               }
           }
           sp++;
       }

       /* Fallback to extralarge if no "large" was found */
       if (s_img_url[0] == '\0' && best_large_url[0] != '\0') {
           strncpy(s_img_url, best_large_url, LASTFM_MAX_IMG_URL_LEN - 1);
           s_img_url[LASTFM_MAX_IMG_URL_LEN - 1] = '\0';
       }
   }

   /* Step 1: Sanitise the Last.fm URL so it works as a fallback even if
    * iTunes fails.  png->jpg so TJpgDec gets a JPEG; https->http to avoid
    * the CC3200 TLS root-CA error (-340). The result will still be a
    * progressive JPEG, but at least it won't crash on connect or parse. */
   {
      int url_len = (int)strlen(s_img_url);
      /* .png -> .jpg */
      if (url_len > 4 &&
          s_img_url[url_len-4] == '.' &&
          s_img_url[url_len-3] == 'p' &&
          s_img_url[url_len-2] == 'n' &&
          s_img_url[url_len-1] == 'g') {
          s_img_url[url_len-3] = 'j';
          s_img_url[url_len-2] = 'p';
          s_img_url[url_len-1] = 'g';
      }
      /* https:// -> http:// */
      if (strncmp(s_img_url, "https://", 8) == 0) {
          memmove(s_img_url + 4, s_img_url + 5,
                  strlen(s_img_url + 5) + 1);
      }
   }

    UART_PRINT("[LastFM] Album art URL: %s\n\r",
               s_img_url[0] ? s_img_url : "(none)");

    // --- Extract genre tags ---
    char tags[UI_MAX_LIST_ITEMS][UI_MAX_LIST_ITEM_LEN];
    int  tag_count = 0;

    // track.getInfo returns tags under "toptags":{"tag":[...]}
    const char *toptags = strstr(body, "\"toptags\":{");
    if (toptags) {
        tag_count = json_get_name_array(toptags, "tag",
                                        tags, LASTFM_MAX_LIST_ITEMS);
    }

    if (tag_count > 0) {
        oled_ui_update_genre_tags(
            (const char (*)[UI_MAX_LIST_ITEM_LEN])tags, tag_count);
        UART_PRINT("[LastFM] track.getInfo: %d tags\n\r", tag_count);
    }

    return LASTFM_OK;
}

// ---------------------------------------------------------------------------
// 6b  artist.getInfo
//
// Fetches artist metadata and fills:
//   - OLED artist bio  (biography summary, HTML stripped)
//   - OLED genre tags  (merged / overwritten with artist-level tags)
// Note: similar artists are populated by query_similar_artists() via a
// dedicated artist.getSimilar call that honours &limit=.
//
// Returns LASTFM_OK on success, LASTFM_ERR_* on failure.
// ---------------------------------------------------------------------------
static int query_artist_info(const char *artist)
{
    char enc_artist[128];
    url_encode(artist, enc_artist, sizeof(enc_artist));

    char path[384];
    snprintf(path, sizeof(path),
             "/2.0/?method=artist.getinfo"
             "&artist=%s"
             "&api_key=%s"
             "&autocorrect=1"
             "&format=json",
             enc_artist, s_api_key);

    int bytes = http_get_plain(LASTFM_API_HOST, LASTFM_API_PORT, path);
    if (bytes < 0) return bytes;

    int status = 0;
    const char *body = http_skip_headers(s_http_buf, bytes, &status);
    if (!body || status != 200) return LASTFM_ERR_HTTP;

    // --- Artist biography (summary field shorter, HTML-stripped) ---
    // The "bio" object has both "summary" (~300 chars) and "content" (full).
    // We use "summary" to stay within UI_MAX_BIO_LEN and within our buffer.
    static char bio[UI_MAX_BIO_LEN];  // static to avoid stack overflow
    bio[0] = '\0';

    const char *bio_obj = strstr(body, "\"bio\":{");
    if (!bio_obj) bio_obj = strstr(body, "\"wiki\":{");  // some responses use "wiki"

    if (bio_obj) {
        if (!json_get_string(bio_obj, "summary", bio, sizeof(bio))) {
            json_get_string(bio_obj, "content", bio, sizeof(bio));
        }
        // Clean up HTML and whitespace
        strip_html(bio);
        collapse_whitespace(bio);
        // Truncate with ellipsis if needed
        if (strlen(bio) >= UI_MAX_BIO_LEN - 4) {
            strcpy(bio + UI_MAX_BIO_LEN - 4, "...");
        }
    }

    if (bio[0] == '\0') {
        strncpy(bio, "No biography available.", sizeof(bio) - 1);
    }

    oled_ui_update_artist_bio(bio);
    UART_PRINT("[LastFM] Bio: %.60s...\n\r", bio);

    // --- Genre tags (from artist typically higher quality than track tags) ---
    char tags[UI_MAX_LIST_ITEMS][UI_MAX_LIST_ITEM_LEN];
    int  tag_count = 0;

    const char *tags_block = strstr(body, "\"tags\":{");
    if (tags_block) {
        tag_count = json_get_name_array(tags_block, "tag",
                                        tags, LASTFM_MAX_LIST_ITEMS);
    }

    if (tag_count > 0) {
        // Artist tags overwrite what track.getInfo set artist tags are richer
        oled_ui_update_genre_tags(
            (const char (*)[UI_MAX_LIST_ITEM_LEN])tags, tag_count);
        UART_PRINT("[LastFM] Artist tags: %d\n\r", tag_count);
    }

    return LASTFM_OK;
}

// ---------------------------------------------------------------------------
// 6c  artist.getSimilar
//
// Dedicated call for similar artists.  artist.getInfo embeds a hard-capped
// list of 5 similar artists in its response regardless of any limit parameter;
// artist.getSimilar is a separate endpoint that honours &limit= and can
// return up to LASTFM_MAX_LIST_ITEMS entries.
//
// Fills OLED_VIEW_SIMILAR_ARTISTS.
// Returns LASTFM_OK on success, LASTFM_ERR_* on failure.
// ---------------------------------------------------------------------------
static int query_similar_artists(const char *artist)
{
    char enc_artist[128];
    url_encode(artist, enc_artist, sizeof(enc_artist));

    char path[384];
    snprintf(path, sizeof(path),
             "/2.0/?method=artist.getsimilar"
             "&artist=%s"
             "&api_key=%s"
             "&autocorrect=1&limit=%d"
             "&format=json",
             enc_artist, s_api_key, LASTFM_MAX_LIST_ITEMS);

    int bytes = http_get_plain(LASTFM_API_HOST, LASTFM_API_PORT, path);
    if (bytes < 0) return bytes;

    int status = 0;
    const char *body = http_skip_headers(s_http_buf, bytes, &status);
    if (!body || status != 200) return LASTFM_ERR_HTTP;

    // Response: {"similarartists":{"artist":[{"name":"...","match":"..."},...]},...}
    char sim_artists[UI_MAX_LIST_ITEMS][UI_MAX_LIST_ITEM_LEN];
    int  sim_count = 0;

    const char *sim_block = strstr(body, "\"similarartists\":{");
    if (sim_block) {
        sim_count = json_get_name_array(sim_block, "artist",
                                        sim_artists, LASTFM_MAX_LIST_ITEMS);
    }

    if (sim_count > 0) {
        oled_ui_update_similar_artists(
            (const char (*)[UI_MAX_LIST_ITEM_LEN])sim_artists, sim_count);
        UART_PRINT("[LastFM] artist.getSimilar: %d results\n\r", sim_count);
    } else {
        const char placeholder[1][UI_MAX_LIST_ITEM_LEN] = {"No similar artists found"};
        oled_ui_update_similar_artists(
            (const char (*)[UI_MAX_LIST_ITEM_LEN])placeholder, 1);
        UART_PRINT("[LastFM] artist.getSimilar: no results\n\r");
    }

    return LASTFM_OK;
}

// ---------------------------------------------------------------------------
// 6d  Similar / top tracks
//
// Attempts track.getSimilar first.  As of 2025, this endpoint often returns
// an empty list; if fewer than 2 results are found, falls back to
// artist.getTopTracks.
//
// Fills OLED_VIEW_SIMILAR_TRACKS.
// Returns LASTFM_OK on success, LASTFM_ERR_* on failure.
// ---------------------------------------------------------------------------
static int query_similar_tracks(const char *artist, const char *track)
{
    char enc_artist[128], enc_track[128];
    url_encode(artist, enc_artist, sizeof(enc_artist));
    url_encode(track,  enc_track,  sizeof(enc_track));

    int   got_results  = 0;
    char  items[UI_MAX_LIST_ITEMS][UI_MAX_LIST_ITEM_LEN];

    // --- Attempt 1: track.getSimilar ---
    {
        char path[384];
        snprintf(path, sizeof(path),
                 "/2.0/?method=track.getsimilar"
                 "&artist=%s&track=%s"
                 "&api_key=%s"
                 "&autocorrect=1&limit=%d"
                 "&format=json",
                 enc_artist, enc_track, s_api_key, LASTFM_MAX_LIST_ITEMS);

        int bytes = http_get_plain(LASTFM_API_HOST, LASTFM_API_PORT, path);
        if (bytes > 0) {
            int status = 0;
            const char *body = http_skip_headers(s_http_buf, bytes, &status);
            if (body && status == 200) {
                // Tracks are under "similartracks":{"track":[...]}
                // Each track object has "name" (title) and nested "artist"."name"
                const char *sim_block = strstr(body, "\"similartracks\":{");
                if (sim_block) {
                    // Extract track names with artist suffix:  "Title, Artist"
                    char track_names[UI_MAX_LIST_ITEMS][UI_MAX_LIST_ITEM_LEN];
                    int count = json_get_name_array(sim_block, "track",
                                                    track_names,
                                                    LASTFM_MAX_LIST_ITEMS);
                    if (count >= 2) {
                        // Also try to pull artist name for each track
                        // For simplicity, just use track names (artist info is nested)
                        int i;
                        for (i = 0; i < count && i < UI_MAX_LIST_ITEMS; i++) {
                            strncpy(items[i], track_names[i],
                                    UI_MAX_LIST_ITEM_LEN - 1);
                            items[i][UI_MAX_LIST_ITEM_LEN - 1] = '\0';
                        }
                        got_results = count;
                        UART_PRINT("[LastFM] track.getSimilar: %d results\n\r", count);
                    }
                }
            }
        }
    }

    // --- Attempt 2: artist.getTopTracks (fallback) ---
    if (got_results < 2) {
        UART_PRINT("[LastFM] track.getSimilar empty/broken; "
                   "using artist.getTopTracks fallback\n\r");

        char path[384];
        snprintf(path, sizeof(path),
                 "/2.0/?method=artist.gettoptracks"
                 "&artist=%s"
                 "&api_key=%s"
                 "&autocorrect=1&limit=%d"
                 "&format=json",
                 enc_artist, s_api_key, LASTFM_MAX_LIST_ITEMS);

        int bytes = http_get_plain(LASTFM_API_HOST, LASTFM_API_PORT, path);
        if (bytes > 0) {
            int status = 0;
            const char *body = http_skip_headers(s_http_buf, bytes, &status);
            if (body && status == 200) {
                const char *top_block = strstr(body, "\"toptracks\":{");
                if (top_block) {
                    got_results = json_get_name_array(top_block, "track",
                                                      items,
                                                      LASTFM_MAX_LIST_ITEMS);
                    UART_PRINT("[LastFM] artist.getTopTracks: %d results\n\r",
                               got_results);
                }
            }
        }
    }

    // --- Update OLED ---
    if (got_results > 0) {
        oled_ui_update_similar_tracks(
            (const char (*)[UI_MAX_LIST_ITEM_LEN])items, got_results);
    } else {
        const char placeholder[1][UI_MAX_LIST_ITEM_LEN] = {"No tracks found"};
        oled_ui_update_similar_tracks(
            (const char (*)[UI_MAX_LIST_ITEM_LEN])placeholder, 1);
    }

    return LASTFM_OK;
}

// ===========================================================================
// 7  JPEG album-cover fetch
// ===========================================================================
//
// SEPARATION OF RESPONSIBILITIES
// --------------------------------
//   lastfm.c (here):
//     - _JpegStream: context struct that holds the open socket + chunk buffers
//     - jpeg_in_cb:  TJpgDec input callback feeds bytes from the socket
//     - parse_url:   splits a "https://host/path" URL into components
//     - LastFM_RenderAlbumCover: opens TLS socket, skips HTTP headers,
//       builds _JpegStream, then calls oled_ui_render_album_jpeg() and
//       closes the socket.
//
//   oled_ui.c (NOT here):
//     - TJpgDec work buffer
//     - jpeg_out_cb: converts RGB888 MCU blockS; RGB565 -> drawPixel()
//     - compute_scale: aspect-ratio fit + letterbox math
//     - jd_prepare() / jd_decomp() call site
//     - oled_ui_render_album_jpeg(): public entry point that owns the decode
// ===========================================================================

// ---------------------------------------------------------------------------
// 7a  _JpegStream + jpeg_in_cb
//
// TJpgDec calls jpeg_in_cb to fill its internal input buffer.
// On the first calls the preamble bytes (received alongside the HTTP headers
// during header-skip) are returned first; subsequent calls read from the
// socket directly.
// ---------------------------------------------------------------------------

typedef struct {
    int   socket;           // open SimpleLink TLS socket
    char *preamble;         // first JPEG bytes read alongside HTTP headers
    int   preamble_len;
    int   preamble_pos;
    char  chunk[512];       // rolling socket receive buffer
    int   chunk_pos;
    int   chunk_len;
} _JpegStream;

static size_t jpeg_in_cb(JDEC *jd, uint8_t *buf, size_t nbytes)
{
    _JpegStream *stream = (_JpegStream *)jd->device;
    size_t total = 0;

    while (total < nbytes) {
        // Drain preamble first (bytes co-received with HTTP headers)
        if (stream->preamble_pos < stream->preamble_len) {
            size_t avail = (size_t)(stream->preamble_len - stream->preamble_pos);
            size_t copy  = (nbytes - total < avail) ? (nbytes - total) : avail;
            if (buf) memcpy(buf + total,
                            stream->preamble + stream->preamble_pos, copy);
            stream->preamble_pos += (int)copy;
            total += copy;
            continue;
        }

        // Drain the current socket chunk
        if (stream->chunk_pos < stream->chunk_len) {
            size_t avail = (size_t)(stream->chunk_len - stream->chunk_pos);
            size_t copy  = (nbytes - total < avail) ? (nbytes - total) : avail;
            if (buf) memcpy(buf + total,
                            stream->chunk + stream->chunk_pos, copy);
            stream->chunk_pos += (int)copy;
            total += copy;
            continue;
        }

        // Fetch next chunk from socket
        int n = sl_Recv(stream->socket, stream->chunk,
                        sizeof(stream->chunk), 0);
        if (n <= 0) break;   // EOF or error
        stream->chunk_len = n;
        stream->chunk_pos = 0;
    }

    return total;
}

// ---------------------------------------------------------------------------
// 7b  URL parser
//
// Splits  "https://hostname/path?query"  or  "http://..."
// into host_out and path_out.
// Returns 1 for HTTPS, 0 for HTTP, -1 on error.
// ---------------------------------------------------------------------------
static int parse_url(const char *url,
                     char *host_out, int host_max,
                     char *path_out, int path_max)
{
    const char *p = url;
    int is_https = 0;

    if      (strncmp(p, "https://", 8) == 0) { is_https = 1; p += 8; }
    else if (strncmp(p, "http://",  7) == 0) { is_https = 0; p += 7; }
    else return -1;

    const char *slash = strchr(p, '/');
    if (!slash) return -1;

    int host_len = (int)(slash - p);
    if (host_len >= host_max) host_len = host_max - 1;
    memcpy(host_out, p, host_len);
    host_out[host_len] = '\0';

    strncpy(path_out, slash, path_max - 1);
    path_out[path_max - 1] = '\0';

    return is_https;
}

// ---------------------------------------------------------------------------
// 7c  LastFM_RenderAlbumCover public implementation
//
// Network responsibility only:
//   1. Parse the cached image URL into host + path
//   2. Open a TLS socket and send an HTTP GET
//   3. Consume the HTTP response headers byte-by-byte
//   4. Stash the first JPEG bytes in _JpegStream.preamble
//   5. Call oled_ui_render_album_jpeg(jpeg_in_cb, &stream)
//       oled_ui.c owns everything from that call onward (decode + draw)
//   6. Close the socket and return
// ---------------------------------------------------------------------------
int LastFM_RenderAlbumCover(void)
{
    if (!s_init) return LASTFM_ERR_NOT_INIT;

    if (s_img_url[0] == '\0') {
        UART_PRINT("[LastFM] No album art URL cached\n\r");
        s_last_error = LASTFM_ERR_PARSE;
        return LASTFM_ERR_PARSE;
    }

    UART_PRINT("[LastFM] Fetching album art: %s\n\r", s_img_url);

    // --- Parse URL ---
    char img_host[80];
    char img_path[256];
    int  is_https = parse_url(s_img_url,
                               img_host, sizeof(img_host),
                               img_path, sizeof(img_path));
    if (is_https < 0) {
        UART_PRINT("[LastFM] Malformed image URL\n\r");
        s_last_error = LASTFM_ERR_PARSE;
        return LASTFM_ERR_PARSE;
    }

    int img_port = is_https ? 443 : 80;

    // --- Open socket and send HTTP GET ---
    unsigned long ulIP = 0;
    long rc = sl_NetAppDnsGetHostByName(
                  (signed char *)img_host,
                  (unsigned short)strlen(img_host),
                  &ulIP, SL_AF_INET);
    if (rc < 0) {
        UART_PRINT("[LastFM] Image DNS failed (%ld)\n\r", rc);
        s_last_error = LASTFM_ERR_DNS;
        return LASTFM_ERR_DNS;
    }

    int sock_type = is_https ? SL_SEC_SOCKET : SL_IPPROTO_TCP;
    int sock = sl_Socket(SL_AF_INET, SL_SOCK_STREAM, sock_type);
    if (sock < 0) {
        s_last_error = LASTFM_ERR_SOCKET;
        return LASTFM_ERR_SOCKET;
    }

    if (is_https) {
        SlSockSecureMethod method;
        method.secureMethod = SL_SO_SEC_METHOD_TLSV1_2;
        sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_SECMETHOD,
                      &method, sizeof(method));

        SlSockSecureMask mask;
        mask.secureMask = SL_SEC_MASK_TLS_RSA_WITH_AES_256_CBC_SHA256
                        | SL_SEC_MASK_TLS_RSA_WITH_AES_128_CBC_SHA256;
        sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_SECURE_MASK,
                      &mask, sizeof(mask));
    }

    SlTimeval_t tv;
    tv.tv_sec  = LASTFM_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (LASTFM_RECV_TIMEOUT_MS % 1000) * 1000;
    sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof(tv));

    SlSockAddrIn_t addr;
    addr.sin_family      = SL_AF_INET;
    addr.sin_port        = sl_Htons((unsigned short)img_port);
    addr.sin_addr.s_addr = sl_Htonl(ulIP);

    rc = sl_Connect(sock, (SlSockAddr_t *)&addr, sizeof(addr));
    if (rc < 0) {
        UART_PRINT("[LastFM] Image connect failed (%ld)\n\r", rc);
        sl_Close(sock);
        s_last_error = LASTFM_ERR_SOCKET;
        return LASTFM_ERR_SOCKET;
    }

    char req[512];
    int  req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: CC3200-FM-Explorer/1.0\r\n"
        "Accept: image/jpeg,image/*\r\n"
        "Connection: close\r\n"
        "\r\n",
        img_path, img_host);

    if (sl_Send(sock, req, req_len, 0) < 0) {
        sl_Close(sock);
        s_last_error = LASTFM_ERR_SEND;
        return LASTFM_ERR_SEND;
    }


    /* Read in JPEG; read headers in bulk chunks of 256 bytes for efficiency */
    int  hdr_total   = 0;
    int  hdr_end     = -1;
    int  scan_from   = 0;
    int  i           = 0;
    int  status      = 0;
    int  preamble_len = 0;
    int  recv_n      = 0;
    _JpegStream stream;
    int  result      = 0;

    UART_PRINT("[LastFM] Reading image headers...\n\r");

    /* Read headers in 256-byte chunks until \r\n\r\n is found */
    while (hdr_total < LASTFM_HTTP_BUF_SIZE - 1 && hdr_end < 0) {
        int space = LASTFM_HTTP_BUF_SIZE - 1 - hdr_total;
        int chunk = (space > 256) ? 256 : space;
        recv_n = sl_Recv(sock, s_http_buf + hdr_total, chunk, 0);
        if (recv_n <= 0) {
            UART_PRINT("[LastFM] sl_Recv error %d while reading headers\n\r", recv_n);
            break;
        }
        hdr_total += recv_n;

        /* Scan newly received bytes for \r\n\r\n */
        scan_from = (hdr_total - recv_n - 3 > 0) ? (hdr_total - recv_n - 3) : 0;
        for (i = scan_from; i <= hdr_total - 4; i++) {
            if (s_http_buf[i]   == '\r' && s_http_buf[i+1] == '\n' &&
                s_http_buf[i+2] == '\r' && s_http_buf[i+3] == '\n') {
                hdr_end = i + 4;
                break;
            }
        }
    }

    UART_PRINT("[LastFM] Header read: %d bytes, separator %s\n\r",
               hdr_total, hdr_end >= 0 ? "found" : "NOT FOUND");

    if (hdr_end < 0) {
        sl_Close(sock);
        s_last_error = LASTFM_ERR_HTTP;
        return LASTFM_ERR_HTTP;
    }

    /* Check HTTP status from the first response line */
    if (strncmp(s_http_buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(s_http_buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    UART_PRINT("[LastFM] Image HTTP %d\n\r", status);
    if (status != 200) {
        sl_Close(sock);
        s_last_error = LASTFM_ERR_HTTP;
        return LASTFM_ERR_HTTP;
    }

    /* Any bytes received after \r\n\r\n are already the JPEG body.
       memmove them to the front of s_http_buf as the preamble. */
    preamble_len = hdr_total - hdr_end;
    if (preamble_len > 0) {
        memmove(s_http_buf, s_http_buf + hdr_end, preamble_len);
    }
    s_http_buf[preamble_len] = '\0';
    UART_PRINT("[LastFM] JPEG preamble: %d bytes co-received\n\r", preamble_len);

    /* If nothing came after the headers, do one bulk read to prime the pump */
    if (preamble_len == 0) {
        recv_n = sl_Recv(sock, s_http_buf, LASTFM_HTTP_BUF_SIZE - 1, 0);
        if (recv_n > 0) {
            preamble_len = recv_n;
            s_http_buf[preamble_len] = '\0';
            UART_PRINT("[LastFM] JPEG preamble (bulk): %d bytes\n\r", preamble_len);
        } else {
            UART_PRINT("[LastFM] Initial JPEG recv returned %d\n\r", recv_n);
        }
    }

    /* Sanity-check: first two bytes must be 0xFF 0xD8 (JPEG SOI marker).
       If we see anything else the CDN served a PNG or an error page. */
    if (preamble_len >= 2) {
        unsigned char b0 = (unsigned char)s_http_buf[0];
        unsigned char b1 = (unsigned char)s_http_buf[1];
        UART_PRINT("[LastFM] SOI: 0x%02X 0x%02X (%s)\n\r",
                   b0, b1,
                   (b0 == 0xFF && b1 == 0xD8) ? "VALID JPEG" : "BAD - not a JPEG");
    }

    /* Hand off to oled_ui for decode + draw */
    stream.socket       = sock;
    stream.preamble     = s_http_buf;
    stream.preamble_len = preamble_len;
    stream.preamble_pos = 0;
    stream.chunk_pos    = 0;
    stream.chunk_len    = 0;

    UART_PRINT("[LastFM] Handing off to TJpgDec...\n\r");

    result = oled_ui_render_album_jpeg((OledJpegInFn)jpeg_in_cb, &stream);

    sl_Close(sock);
    s_last_error = result;
    return result;
}

// ===========================================================================
// 8  Public API implementations
// ===========================================================================

void LastFM_Init(const char *api_key)
{
    if (api_key && strlen(api_key) > 0) {
        strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
        s_api_key[sizeof(s_api_key) - 1] = '\0';
    }
    s_img_url[0] = '\0';
    s_last_error  = LASTFM_OK;
    s_init        = true;
    UART_PRINT("[LastFM] Init complete. API key: %.8s...\n\r", s_api_key);
}

int LastFM_QueryAndUpdateViews(const char *artist, const char *track)
{
    if (!s_init) {
        UART_PRINT("[LastFM] Error: LastFM_Init() not called\n\r");
        s_last_error = LASTFM_ERR_NOT_INIT;
        return LASTFM_ERR_NOT_INIT;
    }

    if (!artist || !track || artist[0] == '\0' || track[0] == '\0') {
        UART_PRINT("[LastFM] Error: artist or track is empty\n\r");
        return LASTFM_ERR_PARSE;
    }

    UART_PRINT("[LastFM] Querying: \"%s\" - \"%s\"\n\r", artist, track);

    // Call LRClib functionality here.
    LRCLib_FetchLyrics(artist, track);

    // Album cover: reset until track.getInfo refreshes the URL
    oled_ui_update_album_cover(false);
    s_img_url[0] = '\0';

    int successes = 0;

    // --- Call 1: track.getInfo ---
    char corrected_artist[UI_MAX_ARTIST_LEN];
    char corrected_track [UI_MAX_SONG_LEN];
    strncpy(corrected_artist, artist, sizeof(corrected_artist) - 1);
    strncpy(corrected_track,  track,  sizeof(corrected_track)  - 1);
    corrected_artist[sizeof(corrected_artist) - 1] = '\0';
    corrected_track [sizeof(corrected_track)  - 1] = '\0';

    int rc = query_track_info(artist, track,
                               corrected_artist, corrected_track,
                               UI_MAX_ARTIST_LEN);
    if (rc == LASTFM_OK) {
        successes++;
        // Notify album cover view that art may be available.
        // Only set when JPEG support is compiled in without it, the view
        // keeps available=false and shows the "unavailable" placeholder,
        // which is correct since LastFM_RenderAlbumCover() will be a no-op.
        if (s_img_url[0] != '\0') {
            oled_ui_update_album_cover(true);
        }
        UART_PRINT("[LastFM] Corrected: \"%s\" - \"%s\"\n\r",
                   corrected_artist, corrected_track);
        // Update the Radio view with the corrected artist/track names
        oled_ui_update_radio(NULL, NULL, corrected_track, corrected_artist, 0, 0);
    } else {
        UART_PRINT("[LastFM] track.getInfo failed (%d)\n\r", rc);
        s_last_error = rc;
    }

    // --- Call 2: artist.getInfo ---
    // Use corrected artist name if available
    const char *query_artist = (corrected_artist[0] != '\0')
                                ? corrected_artist : artist;
    rc = query_artist_info(query_artist);
    if (rc == LASTFM_OK) {
        successes++;
    } else {
        UART_PRINT("[LastFM] artist.getInfo failed (%d)\n\r", rc);
        s_last_error = rc;
    }

    // --- Call 3: similar / top tracks ---
    rc = query_similar_tracks(query_artist,
                               (corrected_track[0] != '\0')
                               ? corrected_track : track);
    if (rc == LASTFM_OK) {
        successes++;
    } else {
        UART_PRINT("[LastFM] track.getSimilar/getTopTracks failed (%d)\n\r", rc);
        s_last_error = rc;
    }

    // --- Call 4: artist.getSimilar ---
    // Dedicated endpoint; unlike artist.getInfo's embedded list this one
    // honours &limit= and can return up to LASTFM_MAX_LIST_ITEMS entries.
    rc = query_similar_artists(query_artist);
    if (rc == LASTFM_OK) {
        successes++;
    } else {
        UART_PRINT("[LastFM] artist.getSimilar failed (%d)\n\r", rc);
        s_last_error = rc;
    }

    UART_PRINT("[LastFM] QueryAndUpdateViews complete: %d/4 calls succeeded\n\r",
               successes);
    return successes;
}

bool LastFM_AlbumArtAvailable(void)
{
    return s_init && (s_img_url[0] != '\0');
}

int LastFM_GetLastError(void)
{
    return s_last_error;
}
