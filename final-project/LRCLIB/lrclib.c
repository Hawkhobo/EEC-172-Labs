// LRCLIB Lyrics API Client Implementation
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// See lrclib.h for full documentation.
//
// SEPARATION OF RESPONSIBILITIES
// --------------------------------
//   lrclib.c  -- network I/O only: DNS, TLS socket, HTTP GET, JSON parsing,
//                LRC timestamp parsing, and synced-display windowing.
//   oled_ui.c -- all pixel rendering: text wrap, scroll, view management.

// ===========================================================================
// 1  Includes & compile-time guards
// ===========================================================================

#include <LRCLIB/lrclib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// SimpleLink / network stack
#include "simplelink.h"

// UART debug
#include "uart_if.h"
#include "common.h"

// OLED UI data update API  (to populate lyrics view)
#include "../OLED_UI/oled_ui.h"

// ===========================================================================
// 2  Internal types
// ===========================================================================

// One parsed line from a "syncedLyrics" LRC string.
// "[MM:SS.xx]text" -> time_ms + text
typedef struct {
    unsigned long time_ms;                 // absolute timestamp in milliseconds
    char          text[LRCLIB_MAX_LINE_LEN]; // lyric text, NUL-terminated
} LrcLine;

// ===========================================================================
// 3  Module state
// ===========================================================================

static bool     s_init         = false;
static int      s_last_error   = LRCLIB_OK;
static bool     s_has_synced   = false;
static int      s_line_count   = 0;
static int      s_current_line = -1;   // -1 = no line selected yet

// Shared HTTP receive buffer.  All callers are sequential; no concurrency.
static char     s_http_buf[LRCLIB_HTTP_BUF_SIZE];

// Parsed synced lyric lines.
static LrcLine  s_lines[LRCLIB_MAX_LYRIC_LINES];

// Plain lyrics text, held for the lifetime of the current song so that
// the lyrics view can be restored if needed.
static char     s_plain_lyrics[UI_MAX_LYRICS_LEN];

// ===========================================================================
// 4  Internal utility helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// 4a  URL encoding
//
// Encodes src into dst (max dst_len bytes including NUL).
// Spaces become '+'; other non-unreserved chars become %XX.
// Mirrors the implementation in lastfm.c.
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
// Returns 1 on success, 0 if the key is not found or the value is not a
// JSON string (e.g. null).
// Handles \n \t \" \\ \/ \r \uXXXX (non-ASCII Unicode -> '?').
// Mirrors the implementation in lastfm.c.
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
    if (*p != '"') return 0;   // value is null or a number, not a string
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
                    // \uXXXX -- use '?' for non-ASCII (OLED font is ASCII only)
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
// 4c  HTTP header skip + status code extraction
//
// Scans resp for the "\r\n\r\n" header terminator.
// Returns a pointer to the first byte of the HTTP body, or NULL if not found.
// Writes the HTTP status code (e.g. 200, 404) into *status_code if non-NULL.
// ---------------------------------------------------------------------------
static const char *http_skip_headers(const char *resp,
                                      int         resp_len,
                                      int        *status_code)
{
    if (status_code) {
        *status_code = 0;
        if (strncmp(resp, "HTTP/", 5) == 0) {
            const char *sp = strchr(resp, ' ');
            if (sp) *status_code = atoi(sp + 1);
        }
    }

    const char *p = resp;
    int remaining = resp_len;

    while (remaining >= 4) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        p++;
        remaining--;
    }
    return NULL;
}

// ===========================================================================
// 5  TLS HTTP GET
//
// Opens a TLS/1.2 connection to LRCLIB_HOST:LRCLIB_PORT, sends a minimal
// HTTP/1.0 GET request for path, and receives the full response into
// s_http_buf.
//
// Using HTTP/1.0 ensures the server cannot respond with chunked
// transfer-encoding (an HTTP/1.1-only feature), so the receive loop simply
// reads until the server closes the connection -- no extra decoding needed.
//
// Certificate validation is intentionally skipped (no CA file loaded) to
// match the behaviour of the album-art TLS code in lastfm.c.  The TLS
// handshake still encrypts the transport.
//
// Returns the number of bytes written to s_http_buf (including headers) on
// success, or a negative LRCLIB_ERR_* code on failure.
// ===========================================================================
static int http_get_tls(const char *path)
{
    // --- DNS resolve ---
    unsigned long ulIP = 0;
    long rc = sl_NetAppDnsGetHostByName(
                  (signed char *)LRCLIB_HOST,
                  (unsigned short)strlen(LRCLIB_HOST),
                  &ulIP, SL_AF_INET);
    if (rc < 0) {
        UART_PRINT("[LRCLIB] DNS failed for %s (rc=%ld)\n\r", LRCLIB_HOST, rc);
        return LRCLIB_ERR_DNS;
    }

    // --- Create TLS socket ---
    int sock = sl_Socket(SL_AF_INET, SL_SOCK_STREAM, SL_SEC_SOCKET);
    if (sock < 0) {
        UART_PRINT("[LRCLIB] Socket create failed (%d)\n\r", sock);
        return LRCLIB_ERR_SOCKET;
    }

    // Force TLS 1.2; CC3200 does not support TLS 1.3
    SlSockSecureMethod method;
    method.secureMethod = SL_SO_SEC_METHOD_TLSV1_2;
    sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_SECMETHOD,
                  &method, sizeof(method));

    // Cipher suite: RSA key exchange with AES-256 or AES-128
    SlSockSecureMask mask;
    mask.secureMask = SL_SEC_MASK_TLS_RSA_WITH_AES_256_CBC_SHA256
                    | SL_SEC_MASK_TLS_RSA_WITH_AES_128_CBC_SHA256;
    sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_SECURE_MASK,
                  &mask, sizeof(mask));

    // Receive timeout
    SlTimeval_t tv;
    tv.tv_sec  = LRCLIB_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (LRCLIB_RECV_TIMEOUT_MS % 1000) * 1000;
    sl_SetSockOpt(sock, SL_SOL_SOCKET, SL_SO_RCVTIMEO, &tv, sizeof(tv));

    // --- Connect ---
    SlSockAddrIn_t addr;
    addr.sin_family      = SL_AF_INET;
    addr.sin_port        = sl_Htons((unsigned short)LRCLIB_PORT);
    addr.sin_addr.s_addr = sl_Htonl(ulIP);

    rc = sl_Connect(sock, (SlSockAddr_t *)&addr, sizeof(addr));
    if (rc < 0) {
        UART_PRINT("[LRCLIB] Connect failed (%ld)\n\r", rc);
        sl_Close(sock);
        return LRCLIB_ERR_SOCKET;
    }

    // --- Build and send HTTP/1.0 GET request ---
    char req[384];
    int  req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: CC3200-FM-Explorer/1.0\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, LRCLIB_HOST);

    if (sl_Send(sock, req, req_len, 0) < 0) {
        UART_PRINT("[LRCLIB] Send failed\n\r");
        sl_Close(sock);
        return LRCLIB_ERR_SEND;
    }

    // --- Receive response into s_http_buf ---
    // HTTP/1.0 + Connection:close: server closes connection after the full
    // response body.  Loop until EOF (n==0) or error/timeout.
    int total = 0;
    int space = LRCLIB_HTTP_BUF_SIZE - 1;

    while (space > 0) {
        int n = sl_Recv(sock, s_http_buf + total, space, 0);
        if (n < 0) {
            // Timeout or error -- treat as EOF if we already have data
            if (total == 0) {
                UART_PRINT("[LRCLIB] Recv failed (%d)\n\r", n);
                sl_Close(sock);
                return LRCLIB_ERR_RECV;
            }
            break;
        }
        if (n == 0) break;   // connection closed by server (normal end)
        total += n;
        space -= n;
    }

    sl_Close(sock);
    s_http_buf[total] = '\0';

    UART_PRINT("[LRCLIB] HTTP GET %s -> %d bytes\n\r", path, total);
    return total;
}

// ===========================================================================
// 6  LRC synced-lyrics parser
//
// Parses the JSON-unescaped "syncedLyrics" value into s_lines[].
// Input format (one entry per text line):
//   "[MM:SS.xx]lyric text\n[MM:SS.xx]lyric text\n..."
//
// Lines that do not begin with '[' are skipped (e.g. empty lead-in markers).
// Returns the number of lines successfully parsed into s_lines[].
// ===========================================================================
static int parse_synced_lyrics(const char *synced)
{
    const char *p = synced;
    int count = 0;

    while (*p && count < LRCLIB_MAX_LYRIC_LINES) {
        // Skip blank lines and stray whitespace between entries
        while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;

        // Every valid LRC line begins with '['
        if (*p != '[') {
            while (*p && *p != '\n') p++;   // skip unexpected content
            continue;
        }
        p++;  // consume '['

        // --- Parse MM ---
        int mm = 0;
        while (*p >= '0' && *p <= '9') { mm = mm * 10 + (*p - '0'); p++; }
        if (*p == ':') p++;

        // --- Parse SS ---
        int ss = 0;
        while (*p >= '0' && *p <= '9') { ss = ss * 10 + (*p - '0'); p++; }

        // --- Parse fractional seconds (centiseconds, up to 2 digits) ---
        int cs = 0;
        if (*p == '.') {
            p++;
            int digits = 0;
            while (*p >= '0' && *p <= '9') {
                if (digits < 2) { cs = cs * 10 + (*p - '0'); digits++; }
                p++;   // consume any extra decimal digits without storing them
            }
            // If only 1 digit was read, cs already represents tenths * 10 ms
        }

        if (*p == ']') p++;  // consume ']'

        // Convert parsed fields to milliseconds
        s_lines[count].time_ms =
            (unsigned long)mm * 60000UL +
            (unsigned long)ss * 1000UL  +
            (unsigned long)cs * 10UL;

        // --- Copy lyric text until end of line or end of string ---
        int i = 0;
        while (*p && *p != '\n' && *p != '\r' &&
               i < LRCLIB_MAX_LINE_LEN - 1) {
            s_lines[count].text[i++] = *p++;
        }
        s_lines[count].text[i] = '\0';

        count++;

        // Advance past the line ending so the next iteration finds '['
        while (*p && *p != '\n') p++;
    }

    return count;
}

// ===========================================================================
// 7  Public API implementations
// ===========================================================================

void LRCLib_Init(void)
{
    s_init            = true;
    s_last_error      = LRCLIB_OK;
    s_has_synced      = false;
    s_line_count      = 0;
    s_current_line    = -1;
    s_plain_lyrics[0] = '\0';
    UART_PRINT("[LRCLIB] Init complete.\n\r");
}

int LRCLib_FetchLyrics(const char *artist, const char *track)
{
    if (!s_init) {
        UART_PRINT("[LRCLIB] Error: LRCLib_Init() not called\n\r");
        s_last_error = LRCLIB_ERR_NOT_INIT;
        return LRCLIB_ERR_NOT_INIT;
    }

    if (!artist || !track || artist[0] == '\0' || track[0] == '\0') {
        UART_PRINT("[LRCLIB] Error: artist or track is empty\n\r");
        s_last_error = LRCLIB_ERR_PARSE;
        return LRCLIB_ERR_PARSE;
    }

    // Reset all state for the incoming song
    s_has_synced      = false;
    s_line_count      = 0;
    s_current_line    = -1;
    s_plain_lyrics[0] = '\0';

    UART_PRINT("[LRCLIB] Querying: \"%s\" - \"%s\"\n\r", artist, track);

    // URL-encode artist and track names
    char enc_artist[128], enc_track[128];
    url_encode(artist, enc_artist, sizeof(enc_artist));
    url_encode(track,  enc_track,  sizeof(enc_track));

    // Build request path: /api/get?artist_name=...&track_name=...
    char path[384];
    snprintf(path, sizeof(path),
             "/api/get?artist_name=%s&track_name=%s",
             enc_artist, enc_track);

    // --- Fetch ---
    int bytes = http_get_tls(path);
    if (bytes < 0) {
        UART_PRINT("[LRCLIB] Network error %d\n\r", bytes);
        s_last_error = bytes;
        oled_ui_update_lyrics(false, NULL);
        return bytes;
    }

    // --- Parse HTTP status ---
    int status = 0;
    const char *body = http_skip_headers(s_http_buf, bytes, &status);

    UART_PRINT("[LRCLIB] HTTP status %d, body offset %d\n\r",
               status, (int)(body ? body - s_http_buf : -1));

    if (!body) {
        UART_PRINT("[LRCLIB] Could not find HTTP header terminator\n\r");
        s_last_error = LRCLIB_ERR_HTTP;
        oled_ui_update_lyrics(false, NULL);
        return LRCLIB_ERR_HTTP;
    }

    if (status == 404) {
        // Track not in the LRCLIB database
        UART_PRINT("[LRCLIB] Track not found (HTTP 404)\n\r");
        s_last_error = LRCLIB_ERR_NOT_FOUND;
        oled_ui_update_lyrics(false, NULL);
        return LRCLIB_ERR_NOT_FOUND;
    }

    if (status != 200) {
        UART_PRINT("[LRCLIB] Unexpected HTTP status %d\n\r", status);
        s_last_error = LRCLIB_ERR_HTTP;
        oled_ui_update_lyrics(false, NULL);
        return LRCLIB_ERR_HTTP;
    }

    // --- Check instrumental flag ---
    // "instrumental":true means the track exists but has no lyrics.
    // Show a short message rather than leaving the view blank.
    if (strstr(body, "\"instrumental\":true") != NULL) {
        UART_PRINT("[LRCLIB] Track is instrumental (no lyrics)\n\r");
        oled_ui_update_lyrics(true, "(Instrumental)");
        s_last_error = LRCLIB_OK;
        return LRCLIB_OK;
    }

    // --- Extract plain lyrics ---
    // plainLyrics is a single JSON string with '\n' as line separators.
    if (!json_get_string(body, "plainLyrics",
                          s_plain_lyrics, sizeof(s_plain_lyrics))) {
        UART_PRINT("[LRCLIB] plainLyrics field not found or null\n\r");
        s_last_error = LRCLIB_ERR_PARSE;
        oled_ui_update_lyrics(false, NULL);
        return LRCLIB_ERR_PARSE;
    }

    UART_PRINT("[LRCLIB] plainLyrics: %d chars\n\r",
               (int)strlen(s_plain_lyrics));

    // Push plain lyrics to the OLED immediately so the view is usable
    // even if synced-lyric parsing below is slow or fails.
    oled_ui_update_lyrics(true, s_plain_lyrics);

    // --- Extract and parse synced lyrics ---
    // syncedLyrics is a JSON string whose value is an LRC-format block.
    // Using a static buffer to avoid a ~3 KB stack allocation on the CC3200.
    static char s_synced_raw[3072];
    s_synced_raw[0] = '\0';

    if (json_get_string(body, "syncedLyrics",
                         s_synced_raw, sizeof(s_synced_raw))) {
        s_line_count = parse_synced_lyrics(s_synced_raw);
        if (s_line_count > 0) {
            s_has_synced = true;
            UART_PRINT("[LRCLIB] Parsed %d synced lyric lines.\n\r",
                       s_line_count);
        } else {
            UART_PRINT("[LRCLIB] syncedLyrics present but 0 lines parsed.\n\r");
        }
    } else {
        UART_PRINT("[LRCLIB] No syncedLyrics in response; plain text only.\n\r");
    }

    s_last_error = LRCLIB_OK;
    return LRCLIB_OK;
}

int LRCLib_UpdateSyncedDisplay(unsigned long elapsed_ms)
{
    if (!s_has_synced || s_line_count <= 0) return 0;

    // --- Binary-search for the last line whose timestamp <= elapsed_ms ---
    // s_lines[] is chronologically ordered, so we scan forward until the
    // next timestamp would overshoot the current playback position.
    int new_line = 0;
    int i;
    for (i = 0; i < s_line_count; i++) {
        if (s_lines[i].time_ms <= elapsed_ms) {
            new_line = i;
        } else {
            break;  // future line; stop here
        }
    }

    // Nothing to do if the active line has not advanced
    if (new_line == s_current_line) return 0;
    s_current_line = new_line;

    // --- Build a display window: current line through end of song ---
    // Concatenate lines from new_line onward, separated by '\n', up to
    // UI_MAX_LYRICS_LEN characters.  The lyrics view will word-wrap and
    // the caller should call oled_ui_reset_scroll() (done here) so the
    // current line appears at the top of the view.
    static char window[UI_MAX_LYRICS_LEN];
    int pos       = 0;
    int remaining = (int)sizeof(window) - 1;

    for (i = new_line; i < s_line_count && remaining > 1; i++) {
        int len = (int)strlen(s_lines[i].text);
        if (len > remaining - 1) len = remaining - 1;
        memcpy(window + pos, s_lines[i].text, (size_t)len);
        pos       += len;
        remaining -= len;
        // Add newline separator between lines (not after the last one)
        if (i < s_line_count - 1 && remaining > 1) {
            window[pos++] = '\n';
            remaining--;
        }
    }
    window[pos] = '\0';

    // Update the OLED lyrics view and scroll back to the top (= current line)
    oled_ui_update_lyrics(true, window);
    oled_ui_reset_scroll();

    return 1;  // line changed; caller should call oled_ui_render()
}

bool LRCLib_HasSyncedLyrics(void)
{
    return s_has_synced && (s_line_count > 0);
}

int LRCLib_GetLastError(void)
{
    return s_last_error;
}
