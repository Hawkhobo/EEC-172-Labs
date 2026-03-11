/*
 * tjpgdcnf.h  —  TJpgDec configuration for CC3200 FM Radio Explorer
 *
 * These settings are tuned for the CC3200's 256 KB RAM budget.
 *
 * Reference: http://elm-chan.org/fsw/tjpgd/
 */
#ifndef TJPGDCNF_H
#define TJPGDCNF_H

/*---------------------------------------------------------------------------
 * JD_SZBUF — Size of the input stream buffer in bytes.
 * TJpgDec calls the input callback to fill this buffer.  Smaller = less RAM;
 * 512 bytes is a good balance for socket reads and the CC3200's constraints.
 *--------------------------------------------------------------------------*/
#define JD_SZBUF        512

/*---------------------------------------------------------------------------
 * JD_FORMAT — Output pixel format.
 *   0 = RGB888  (3 bytes per pixel — used by our jpeg_out_cb)
 *   1 = RGB565  (2 bytes per pixel, little-endian)
 * Keep at 0; our output callback converts to RGB565 for the SSD1351.
 *--------------------------------------------------------------------------*/
#define JD_FORMAT       0

/*---------------------------------------------------------------------------
 * JD_USE_SCALE — Enable the 1/2, 1/4, 1/8 downscale feature.
 * Required: we pass scale=1 for 174x174 images to get 87x87 before NN scale.
 *--------------------------------------------------------------------------*/
#define JD_USE_SCALE    1

/*---------------------------------------------------------------------------
 * JD_TBLCLIP — don't use a 256-byte clipping table instead of conditional jumps.
 * Saves ~20 CPU cycles per component per pixel; just above CC3200's memory budget :(
 *--------------------------------------------------------------------------*/
#define JD_TBLCLIP      0

/*---------------------------------------------------------------------------
 * JD_FASTDECODE — Controls the speed/RAM trade-off for Huffman decoding.
 *   0 = minimal RAM, slowest decode
 *   1 = 128-entry lookup table (recommended — good balance for CC3200)
 *   2 = 1024-entry lookup table (faster, uses more RAM)
 * Use 1 to keep RAM usage predictable on the CC3200's 256 KB budget.
 *--------------------------------------------------------------------------*/
#define JD_FASTDECODE   1

#endif /* TJPGDCNF_H */
