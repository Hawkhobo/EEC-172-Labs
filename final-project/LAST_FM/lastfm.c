// LastFM API Client Implementation
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// See lastfm.h for full documentation.
//
// SEPARATION OF RESPONSIBILITIES
// --------------------------------
//   lastfm.c  -- network I/O only: DNS, sockets, HTTP/TLS, JSON parsing,
//                and downloading the JPEG body into s_jpeg_buf
//   oled_ui.c -- all pixel drawing: stbi_load_from_memory, scale computation,
//                RGB888->RGB565 conversion, drawPixel() loop

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
//#include "../LRCLIB/lrclib.h"


// ===========================================================================
// 2  Internal constants & macros
// ===========================================================================

// Last.fm JSON API base path template (used by query_* helpers)
#define _API_PATH_BASE   "/2.0/?method="

// ===========================================================================
// 3  Module state
// ===========================================================================

static bool   s_init              = false;
static char   s_api_key[48]       = {0};
static char   s_img_url[LASTFM_MAX_IMG_URL_LEN] = {0};  // cached art URL
static int    s_last_error        = LASTFM_OK;
static int    s_track_duration_ms = 0;  // cached from last track.getInfo call

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

// Similar to json_get_name_array, but goes deeper to retrieve both the track and associated artist
static int json_get_track_array(const char *json_start,
                                 const char *arr_key,
                                 char        items[][UI_MAX_LIST_ITEM_LEN],
                                 int         max_items)
{
    char search[72];
    snprintf(search, sizeof(search), "\"%s\":[", arr_key);

    const char *p = strstr(json_start, search);
    if (!p) return 0;
    p += strlen(search);

    int  count = 0;
    char obj_buf[512];

    while (count < max_items && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' ||
               *p == '\n' || *p == ',') p++;

        if (*p == ']') break;
        if (*p != '{') { p++; continue; }

        const char *obj_start = p;
        int depth = 0;
        const char *q = p;
        while (*q) {
            if      (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }

        int obj_len = (int)(q - obj_start);
        if (obj_len >= (int)sizeof(obj_buf))
            obj_len = (int)sizeof(obj_buf) - 1;
        memcpy(obj_buf, obj_start, (size_t)obj_len);
        obj_buf[obj_len] = '\0';

        char title[UI_MAX_LIST_ITEM_LEN];
        title[0] = '\0';
        json_get_string(obj_buf, "name", title, sizeof(title));
        if (title[0] == '\0') { p = q; continue; }

        char artist_name[UI_MAX_LIST_ITEM_LEN];
        artist_name[0] = '\0';

        const char *art_obj = strstr(obj_buf, "\"artist\":{");
        if (art_obj) {
            char art_buf[256];
            const char *a = art_obj + (int)strlen("\"artist\":");
            int adepth = 0;
            const char *ae = a;
            while (*ae) {
                if      (*ae == '{') adepth++;
                else if (*ae == '}') { adepth--; if (adepth == 0) { ae++; break; } }
                ae++;
            }
            int art_len = (int)(ae - a);
            if (art_len >= (int)sizeof(art_buf)) art_len = (int)sizeof(art_buf) - 1;
            memcpy(art_buf, a, (size_t)art_len);
            art_buf[art_len] = '\0';
            json_get_string(art_buf, "name", artist_name, sizeof(artist_name));
        }

        if (artist_name[0] != '\0') {
            snprintf(items[count], UI_MAX_LIST_ITEM_LEN,
                     "%s - %s", title, artist_name);
        } else {
            strncpy(items[count], title, UI_MAX_LIST_ITEM_LEN - 1);
            items[count][UI_MAX_LIST_ITEM_LEN - 1] = '\0';
        }
        count++;
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
    // HTTP/1.0 requested -- some CDNs (e.g. Fastly) ignore this and respond
    // HTTP/1.1 chunked anyway; the caller must handle chunked bodies.
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

    // --- Extract track duration ---
    // Last.fm returns "duration":"243000" (milliseconds as a decimal string).
    // Reset to 0 first so that a missing field doesn't leave a stale value.
    {
        char dur_str[16];
        dur_str[0] = '\0';
        s_track_duration_ms = 0;
        if (json_get_string(body, "duration", dur_str, sizeof(dur_str))) {
            s_track_duration_ms = atoi(dur_str);
        }
        UART_PRINT("[LastFM] Track duration: %d ms\n\r", s_track_duration_ms);
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
               /* Prefer "medium" (64x64): small thumbnails are almost always
                * stored as baseline JPEG.  "large" (174x174) and "extralarge"
                * (300x300) from the Last.fm CDN are progressive JPEGs, which
                * TJpgDec (R0.03) cannot decode.
                * Priority: medium > small > large > extralarge */
               if (strcmp(size_val, "medium") == 0) {
                   /* Best option -- take it immediately */
                   strncpy(s_img_url, candidate_url, LASTFM_MAX_IMG_URL_LEN - 1);
                   s_img_url[LASTFM_MAX_IMG_URL_LEN - 1] = '\0';
                   break;
               } else if (best_large_url[0] == '\0') {
                   /* Keep first non-medium URL as fallback */
                   strncpy(best_large_url, candidate_url, LASTFM_MAX_IMG_URL_LEN - 1);
                   best_large_url[LASTFM_MAX_IMG_URL_LEN - 1] = '\0';
               }
           }
           sp++;
       }

       /* Fallback to whatever size was found if no "medium" was found */
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
                    int count = json_get_track_array(sim_block, "track",
                                                     items,
                                                     LASTFM_MAX_LIST_ITEMS);
                    if (count >= 2) {
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
                    got_results = json_get_track_array(top_block, "track",
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
// lastfm.c: download full HTTP response into s_jpeg_buf, strip headers,
//           then call oled_ui_render_album_jpeg(s_jpeg_buf, img_len).
// oled_ui.c: stbi_load_from_memory() + NN scale + drawPixel().
// ===========================================================================

/* Full HTTP response (headers + JPEG body) lands here before any decoding. */
static unsigned char s_jpeg_buf[LASTFM_JPEG_BUF_SIZE];

// ---------------------------------------------------------------------------
// 7a  parse_url
//
// Splits  "https://hostname/path"  or  "http://hostname/path"
// into host_out and path_out.
// Returns 1 for HTTPS, 0 for HTTP, -1 on error.
// ---------------------------------------------------------------------------
static int parse_url(const char *url,
                     char *host_out, int host_max,
                     char *path_out, int path_max)
{
    const char *p = url;
    const char *slash;
    int         is_https;
    int         host_len;

    if      (strncmp(p, "https://", 8) == 0) { is_https = 1; p += 8; }
    else if (strncmp(p, "http://",  7) == 0) { is_https = 0; p += 7; }
    else return -1;

    slash = strchr(p, '/');
    if (!slash) return -1;

    host_len = (int)(slash - p);
    if (host_len >= host_max) host_len = host_max - 1;
    memcpy(host_out, p, (size_t)host_len);
    host_out[host_len] = '\0';

    strncpy(path_out, slash, (size_t)(path_max - 1));
    path_out[path_max - 1] = '\0';

    return is_https;
}

// ---------------------------------------------------------------------------
// 7b  LastFM_RenderAlbumCover -- public implementation
//
// Steps:
//   1. Parse cached image URL into host + path
//   2. DNS resolve + open plain TCP socket
//      (query_track_info strips https:// -> http:// so no TLS needed here)
//   3. Send HTTP GET; receive the FULL response into s_jpeg_buf
//   4. Locate \r\n\r\n; verify HTTP 200; memmove body to front of buffer
//   4. Locate \r\n\r\n; verify HTTP 200; memmove body to front of buffer
//   5. Call oled_ui_render_album_jpeg(s_jpeg_buf, img_len)
// ---------------------------------------------------------------------------
int LastFM_RenderAlbumCover(void)
{
    char           img_host[80];
    char           img_path[256];
    int            is_https;
    unsigned long  ulIP;
    long           rc;
    int            sock;
    SlTimeval_t    tv;
    SlSockAddrIn_t addr;
    char           req[512];
    int            req_len;
    int            total;
    int            space;
    int            n;
    int            i;
    int            hdr_end;
    int            img_len;
    int            status;
    int            result;

    if (!s_init) return LASTFM_ERR_NOT_INIT;

    if (s_img_url[0] == '\0') {
        UART_PRINT("[LastFM] No album art URL cached\n\r");
        s_last_error = LASTFM_ERR_PARSE;
        return LASTFM_ERR_PARSE;
    }

    UART_PRINT("[LastFM] Fetching: %s\n\r", s_img_url);

    /* 1. Parse URL */
    is_https = parse_url(s_img_url,
                         img_host, sizeof(img_host),
                         img_path, sizeof(img_path));
    if (is_https < 0) {
        UART_PRINT("[LastFM] Malformed image URL\n\r");
        s_last_error = LASTFM_ERR_PARSE;
        return LASTFM_ERR_PARSE;
    }

    /* 2. DNS */
    ulIP = 0;
    rc = sl_NetAppDnsGetHostByName(
             (signed char *)img_host,
             (unsigned short)strlen(img_host),
             &ulIP, SL_AF_INET);
    if (rc < 0) {
        UART_PRINT("[LastFM] Image DNS failed (%ld)\n\r", rc);
        s_last_error = LASTFM_ERR_DNS;
        return LASTFM_ERR_DNS;
    }

    /* 3. Open plain TCP socket */
    sock = sl_Socket(SL_AF_INET, SL_SOCK_STREAM, SL_IPPROTO_TCP);
    if (sock < 0) {
        s_last_error = LASTFM_ERR_SOCKET;
        return LASTFM_ERR_SOCKET;
    }

    tv.tv_sec  = LASTFM_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (LASTFM_RECV_TIMEOUT_MS % 1000) * 1000;
    sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof(tv));

    addr.sin_family      = SL_AF_INET;
    addr.sin_port        = sl_Htons((unsigned short)(is_https ? 443 : 80));
    addr.sin_addr.s_addr = sl_Htonl(ulIP);

    rc = sl_Connect(sock, (SlSockAddr_t *)&addr, sizeof(addr));
    if (rc < 0) {
        UART_PRINT("[LastFM] Image connect failed (%ld)\n\r", rc);
        sl_Close(sock);
        s_last_error = LASTFM_ERR_SOCKET;
        return LASTFM_ERR_SOCKET;
    }

    /* 4. Send HTTP GET */
    req_len = snprintf(req, sizeof(req),
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

    /* 5. Receive the full HTTP response into s_jpeg_buf */
    total = 0;
    space = LASTFM_JPEG_BUF_SIZE - 1;
    while (space > 0) {
        n = sl_Recv(sock, s_jpeg_buf + total, space, 0);
        if (n < 0) {
            if (total == 0) {
                UART_PRINT("[LastFM] Recv error (%d)\n\r", n);
                sl_Close(sock);
                s_last_error = LASTFM_ERR_RECV;
                return LASTFM_ERR_RECV;
            }
            break; /* partial data: timeout after bytes received, treat as done */
        }
        if (n == 0) break; /* server closed connection (normal for HTTP/1.0) */
        total += n;
        space -= n;
    }
    sl_Close(sock);
    UART_PRINT("[LastFM] Received %d bytes\n\r", total);

    /* 6. Find end of HTTP headers */
    hdr_end = -1;
    for (i = 0; i <= total - 4; i++) {
        if (s_jpeg_buf[i]   == '\r' && s_jpeg_buf[i+1] == '\n' &&
            s_jpeg_buf[i+2] == '\r' && s_jpeg_buf[i+3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    }
    if (hdr_end < 0) {
        UART_PRINT("[LastFM] HTTP header end not found\n\r");
        s_last_error = LASTFM_ERR_HTTP;
        return LASTFM_ERR_HTTP;
    }

    /* 7. Extract HTTP status code */
    status = 0;
    {
        const char *sp = strchr((const char *)s_jpeg_buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    UART_PRINT("[LastFM] Image HTTP %d\n\r", status);
    if (status != 200) {
        s_last_error = LASTFM_ERR_HTTP;
        return LASTFM_ERR_HTTP;
    }

    /* 8. Strip HTTP headers: memmove body to front of s_jpeg_buf */
    img_len = total - hdr_end;
    if (img_len <= 2) {
        UART_PRINT("[LastFM] Empty image body\n\r");
        s_last_error = LASTFM_ERR_HTTP;
        return LASTFM_ERR_HTTP;
    }
    memmove(s_jpeg_buf, s_jpeg_buf + hdr_end, (size_t)img_len);
    UART_PRINT("[LastFM] JPEG body: %d bytes\n\r", img_len);

    /* Diagnostic: log first 8 bytes so we can confirm JPEG SOI or spot
     * chunked encoding (which starts with a hex size line, not 0xFF 0xD8). */
    if (img_len >= 8) {
        UART_PRINT("[LastFM] Body[0..7]: %02X %02X %02X %02X %02X %02X %02X %02X\n\r",
                   s_jpeg_buf[0], s_jpeg_buf[1], s_jpeg_buf[2], s_jpeg_buf[3],
                   s_jpeg_buf[4], s_jpeg_buf[5], s_jpeg_buf[6], s_jpeg_buf[7]);
    }

    /* Handle HTTP/1.1 chunked transfer encoding.
     * Even though we request HTTP/1.0, CDNs like Fastly may respond with
     * HTTP/1.1 and Transfer-Encoding: chunked.  In that case the body starts
     * with a hex chunk-size line (e.g. "63f\r\n") rather than 0xFF 0xD8.
     * We detect this by checking whether the response is HTTP/1.1 AND the
     * first byte is NOT 0xFF (the JPEG SOI start byte).
     * A minimal single-chunk decode is sufficient: Last.fm thumbnails arrive
     * in one chunk followed by "0\r\n\r\n". */
    if (s_jpeg_buf[0] != 0xFF) {
        /* Likely chunked: accumulate decoded bytes into a local region.
         * We decode in-place by walking the existing s_jpeg_buf. */
        int        rd  = 0;   /* read cursor into s_jpeg_buf  */
        int        wr  = 0;   /* write cursor (decoded output) */
        int        chunk_sz;
        const char *end_ptr;

        UART_PRINT("[LastFM] Chunked body detected -- decoding\n\r");

        while (rd < img_len) {
            /* Parse hex chunk size (terminated by \r\n) */
            chunk_sz = (int)strtol((const char *)s_jpeg_buf + rd, (char **)&end_ptr, 16);
            if (end_ptr == (const char *)s_jpeg_buf + rd) break; /* no hex digits */
            rd = (int)(end_ptr - (const char *)s_jpeg_buf);

            /* Skip the trailing \r\n after the size */
            if (rd + 1 < img_len &&
                s_jpeg_buf[rd] == '\r' && s_jpeg_buf[rd+1] == '\n') {
                rd += 2;
            }

            if (chunk_sz == 0) break; /* terminal chunk */

            /* Bounds check */
            if (rd + chunk_sz > img_len || wr + chunk_sz > LASTFM_JPEG_BUF_SIZE) {
                UART_PRINT("[LastFM] Chunk overrun rd=%d sz=%d\n\r", rd, chunk_sz);
                break;
            }

            /* Copy chunk data to write position (may overlap if wr < rd) */
            memmove(s_jpeg_buf + wr, s_jpeg_buf + rd, (size_t)chunk_sz);
            wr  += chunk_sz;
            rd  += chunk_sz;

            /* Skip trailing \r\n after chunk data */
            if (rd + 1 < img_len &&
                s_jpeg_buf[rd] == '\r' && s_jpeg_buf[rd+1] == '\n') {
                rd += 2;
            }
        }

        img_len = wr;
        UART_PRINT("[LastFM] Chunked decode: %d bytes\n\r", img_len);

        /* Re-print first bytes after decode to confirm 0xFF 0xD8 */
        if (img_len >= 4) {
            UART_PRINT("[LastFM] After decode[0..3]: %02X %02X %02X %02X\n\r",
                       s_jpeg_buf[0], s_jpeg_buf[1],
                       s_jpeg_buf[2], s_jpeg_buf[3]);
        }
    }

    if (img_len <= 2 || s_jpeg_buf[0] != 0xFF || s_jpeg_buf[1] != 0xD8) {
        UART_PRINT("[LastFM] Not a JPEG after decode (%02X %02X)\n\r",
                   s_jpeg_buf[0], s_jpeg_buf[1]);
        s_last_error = LASTFM_ERR_JPEG;
        return LASTFM_ERR_JPEG;
    }

    /* Pass raw JPEG bytes to oled_ui for decode and draw.
     * stb_image handles both baseline and progressive JPEGs. */
    UART_PRINT("[LastFM] Handing %d bytes to stb_image...\n\r", img_len);
    result = oled_ui_render_album_jpeg(s_jpeg_buf, img_len);

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
    //LRCLib_FetchLyrics(artist, track);

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

int LastFM_GetTrackDurationMs(void)
{
    return s_track_duration_ms;
}

int LastFM_GetLastError(void)
{
    return s_last_error;
}
