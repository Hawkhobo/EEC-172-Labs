// Final Project -- FM Radio Exlporer
// Jacob Feenstra & Chun Ho Chen

//*****************************************************************************
//
// Copyright (C) 2014 Texas Instruments Incorporated - http://www.ti.com/ 
// 
// 
//  Redistribution and use in source and binary forms, with or without 
//  modification, are permitted provided that the following conditions 
//  are met:
//
//    Redistributions of source code must retain the above copyright 
//    notice, this list of conditions and the following disclaimer.
//
//    Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the 
//    documentation and/or other materials provided with the   
//    distribution.
//
//    Neither the name of Texas Instruments Incorporated nor the names of
//    its contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
//  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
//  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
//  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
//  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
//  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
//  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
//  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//*****************************************************************************
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// SimpleLink / Network stack (WiFi connect and TLS)
#include "simplelink.h"
#include "utils/network_utils.h"


// Driverlib includes
#include "gpio.h"
#include "hw_types.h"
#include "hw_ints.h"
#include "hw_apps_rcm.h"
#include "hw_common_reg.h"
#include "hw_memmap.h"
#include "interrupt.h"
#include "prcm.h"
#include "rom.h"
#include "rom_map.h"
#include "spi.h"
#include "systick.h"
#include "timer.h"
#include "uart.h"
#include "utils.h"

// Common interface includes
#include "common.h"
#include "Debug/syscfg/pin_mux_config.h"
#include "timer_if.h"
#include "uart_if.h"

// Network utils (WiFi connect + TLS)
#include "utils/network_utils.h"

// for Adafruit_init()
#include "adafruit_oled_lib/Adafruit_SSD1351.h"

// API for FM Radio Explorer
#include "OLED_UI/oled_ui.h"
#include "TEA5767/tea5767.h"
#include "IR_REMOTE_INPUT/ir_remote_input.h"
#include "LAST_FM/lastfm.h"
#include "TSOP311_IR_RECEIVER/tsop311_ir_receiver.h"

#define UI_DEBUG

#ifdef UI_DEBUG
#include "DEMO/demo_code.h"
#endif

#include "LYRICS/lyrics_data.h"

// Last.fm API key -- register at https://www.last.fm/api/account/create
#define LASTFM_API_KEY        "21c2eee2edef4c433f750fedbb43fa94"

// ---------------------------------------------------------------------------
// Lyric-sync / progress-bar timer (TIMERA1, 100 ms periodic)
// ---------------------------------------------------------------------------
// TIMERA1 is available: the IR receiver owns SysTick and GPIOA1, not any
// of the four CC3200 hardware timers.
//
// g_ms_tick is a monotonic millisecond counter incremented by the ISR.
// It is 32-bit unsigned and wraps after ~49 days -- safe for subtraction.
// g_track_start_ms is snapshotted at the end of each query_lastfm() call
// so that (g_ms_tick - g_track_start_ms) gives elapsed time for the track.
// ---------------------------------------------------------------------------
#define LYRICS_TICK_MS      100u                    /* ISR period (ms)      */
#define LYRICS_TICK_RELOAD  (LYRICS_TICK_MS * 80000u) /* ticks at 80 MHz   */

volatile uint32_t g_ms_tick       = 0u;
static   uint32_t g_track_start_ms = 0u;

static void LyricsTimerISR(void)
{
    MAP_TimerIntClear(TIMERA1_BASE, TIMER_TIMA_TIMEOUT);
    g_ms_tick += LYRICS_TICK_MS;
}

//*****************************************************************************
//                      Global Variables for Vector Table
//*****************************************************************************
#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif

//*****************************************************************************
//
//! Board Initialization & Configuration
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void
BoardInit(void)
{
/* In case of TI-RTOS vector table is initialize by OS itself */
#ifndef USE_TIRTOS
  //
  // Set vector table base
  //
#if defined(ccs)
    MAP_IntVTableBaseSet((unsigned long)&g_pfnVectors[0]);
#endif
#if defined(ewarm)
    MAP_IntVTableBaseSet((unsigned long)&__vector_table);
#endif
#endif
    //
    // Enable Processor
    //
    MAP_IntMasterEnable();
    MAP_IntEnable(FAULT_SYSTICK);
    PRCMCC3200MCUInit();
}

//*****************************************************************************
//
//!
//!
//! Configure SPI for communication
//!
//! \return None.
//
//*****************************************************************************
#define SPI_IF_BIT_RATE  3000000
void SPIConfig()
{

    //
    // Reset SPI
    //
    MAP_SPIReset(GSPI_BASE);

    //
    // Configure SPI interface
    //
    // Using Mode 3; only interested in MOSI, CS, and SCLK for SPI
    MAP_SPIConfigSetExpClk(GSPI_BASE,MAP_PRCMPeripheralClockGet(PRCM_GSPI),
                     SPI_IF_BIT_RATE,SPI_MODE_MASTER,SPI_SUB_MODE_3,
                     (SPI_SW_CTRL_CS |
                     SPI_4PIN_MODE |
                     SPI_TURBO_OFF |
                     SPI_CS_ACTIVEHIGH |
                     SPI_WL_8));

    //
    // Enable SPI for communication
    //
    MAP_SPIEnable(GSPI_BASE);

    // enable internal SPI CS to satisfy SPI API;note we are using GPIOP CS instead to work with writeCommand() and writeData()
    MAP_SPICSEnable(GSPI_BASE);

}

static float g_current_freq = 90.3f;   // Initialised to boot frequency
static float g_last_freq    = 100.5f;   // Previous station (same as current at boot)
static bool  g_is_muted     = false;   // Shadow of current mute state


// Format a frequency float as "XX.X FM" or "XXX.X FM" into dst[].
// dst must be at least UI_MAX_STATION_LEN bytes.
static void format_station(float freq, char *dst)
{
    // TEA5767 supports at most 0.1 MHz resolution (PLL step), so one decimal
    // place is always sufficient and keeps the label short.
    snprintf(dst, UI_MAX_STATION_LEN, "%.1f FM", (double)freq);
}

// Tune to freq_mhz, update globals, refresh OLED radio view.
// Returns the TEA5767 result code.
static void query_lastfm(void)
{
    int         calls;
    const char *artist;
    const char *track;

    #ifdef UI_DEBUG
        Demo_GetNextMetadata(&artist, &track);
        UART_PRINT("[UI_DEBUG][LastFM] Querying: '%s' by '%s'\n\r", track, artist);
        calls = LastFM_QueryAndUpdateViews(artist, track);
        UART_PRINT("[UI_DEBUG][LastFM] %d API call(s) succeeded\n\r", calls);

        oled_ui_update_album_cover(LastFM_AlbumArtAvailable());

        {
            const LyricsEntry *le = LyricsData_Find(artist, track);
            if (le && !le->instrumental && le->synced_lyrics) {
                oled_ui_update_lyrics(true, le->synced_lyrics);
                UART_PRINT("[UI_DEBUG][Lyrics] Found: '%s'\n\r", track);
            } else {
                oled_ui_update_lyrics(false, NULL);
                UART_PRINT("[UI_DEBUG][Lyrics] None for '%s' (%s)\n\r",
                           track,
                           (le && le->instrumental) ? "instrumental" : "not found");
            }
        }

        /* Snapshot the timer so elapsed time is measured from this point.
         * This is set AFTER oled_ui_update_lyrics() resets the lyric index,
         * so the first oled_ui_tick() call sees a clean slate. */
        g_track_start_ms = g_ms_tick;
        UART_PRINT("[UI_DEBUG][Sync] Track start ms=%lu, duration=%d ms\n\r",
                   (unsigned long)g_track_start_ms,
                   LastFM_GetTrackDurationMs());

        oled_ui_set_view(OLED_VIEW_RADIO);
        oled_ui_render();
    // Production environment
    #else
        // -----------------------------------------------------------------
        // TODO: replace with live RDS-decoded artist / track strings
        //       once RDS implementation is complete. (Bah Humbug)
        // -----------------------------------------------------------------
        artist = "Tame Impala";
        track  = "Let It Happen";

        UART_PRINT("[LastFM] Querying: '%s' by '%s'\n\r", track, artist);
        calls = LastFM_QueryAndUpdateViews(artist, track);
        UART_PRINT("[LastFM] %d API call(s) succeeded\n\r", calls);

        oled_ui_update_album_cover(LastFM_AlbumArtAvailable());

        {
            const LyricsEntry *le = LyricsData_Find(artist, track);
            if (le && !le->instrumental && le->synced_lyrics) {
                oled_ui_update_lyrics(true, le->synced_lyrics);
            } else {
                oled_ui_update_lyrics(false, NULL);
            }
        }

        /* Snapshot the timer after oled_ui_update_lyrics() resets the index */
        g_track_start_ms = g_ms_tick;

        if (calls > 0) {
            oled_ui_set_view(OLED_VIEW_RADIO);
            oled_ui_render();
        }
    #endif
}

// Tune to freq_mhz, update globals, refresh OLED radio view.
// Returns the TEA5767 result code.
static int tune_and_update(float freq_mhz)
{

    int  rc;
    int  sig;
    char station_str[UI_MAX_STATION_LEN];

    // Determine if input is out of US frequency range, reprompt if so
    if (freq_mhz < TEA5767_BAND_MIN_MHZ || freq_mhz > TEA5767_BAND_MAX_MHZ) {
        UART_PRINT("Out-of-range: %.1f MHz (valid: %.1f-%.1f)\n\r",
                   (double)freq_mhz,
                   (double)TEA5767_BAND_MIN_MHZ,
                   (double)TEA5767_BAND_MAX_MHZ);
        oled_ui_flash_error_banner();
        format_station(g_current_freq, station_str);
        oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, 0);
        oled_ui_set_view(OLED_VIEW_RADIO);
        oled_ui_render();
        return TEA5767_ERR_FREQ;
    }

    rc = TEA5767_TuneFrequency(freq_mhz);
    if (rc == TEA5767_OK || rc == TEA5767_ERR_NO_SIGNAL) {
        g_last_freq    = g_current_freq;
        g_current_freq = freq_mhz;
        format_station(g_current_freq, station_str);
        UART_PRINT("Tuned to %.1f FM\n\r", (double)g_current_freq);

        // Read signal level for the OLED signal-strength bars
        sig = TEA5767_GetSignalStrength();
        if (sig < 0) sig = 0;   // I2C error -- show zero rather than crash

        // Kick off LastFM metadata fetch
        query_lastfm();

        oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, sig);
    } else {
        #ifdef UI_DEBUG
                // Debug mode: ignore tune failure, advance freq and demo playlist anyway
                g_last_freq    = g_current_freq;
                g_current_freq = freq_mhz;
                format_station(g_current_freq, station_str);
                UART_PRINT("[UI_DEBUG] Tune failed (rc=%d), advancing demo track anyway\n\r", rc);
                sig = 0;
                query_lastfm();
                oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, sig);
        #else
                format_station(g_current_freq, station_str);
                UART_PRINT("Tune failed (rc=%d) for %.1f\n\r", rc, (double)freq_mhz);
                sig = TEA5767_GetSignalStrength();
                if (sig < 0) sig = 0;
                oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, sig);
        #endif
    }

    oled_ui_set_view(OLED_VIEW_RADIO);
    oled_ui_render();
    return rc;
}

//*****************************************************************************
//
//! main
//!
//! Initialize peripherals then enter event loop. Poll the
//! IR receiver and dispatch button presses to the OLED UI and radio driver.
//
//*****************************************************************************
int main(void) {
    // Hardware init
   BoardInit();
   PinMuxConfig();

   // Set up SimpleLink WiFi
   long lRetVal = connectToAccessPoint();
    if (lRetVal < 0) {
        UART_PRINT("Wi-Fi connection failed\n\r");
        return 1;
    }
    UART_PRINT("Network Ready\n\r");

   // SPI for OLED, happens after network is configured over SimpleLink (since sl depends on SPI for configuration)
   SPIConfig();

   // OLED display
   Adafruit_Init();
   oled_ui_init();

   // UART Debug
   InitTerm();
   ClearTerm();
   UART_PRINT("FM Radio Explorer ready\n\r");

   // FM Radio
   TEA5767_Init();
   int rc = TEA5767_TuneFrequency(90.3f);
   UART_PRINT("TEA5767 result: %d\n\r", rc);
   oled_ui_update_radio("100.5 FM", "", "", "", 0, 0);
   oled_ui_render();

   // IR Receiver
   IR_Init();

   // TIMERA1: 100 ms periodic timer for progress bar + lyric sync
   MAP_PRCMPeripheralClkEnable(PRCM_TIMERA1, PRCM_RUN_MODE_CLK);
   while (!MAP_PRCMPeripheralStatusGet(PRCM_TIMERA1));
   MAP_TimerConfigure(TIMERA1_BASE, TIMER_CFG_PERIODIC);
   MAP_TimerLoadSet(TIMERA1_BASE, TIMER_A, LYRICS_TICK_RELOAD - 1u);
   MAP_TimerIntRegister(TIMERA1_BASE, TIMER_A, LyricsTimerISR);
   MAP_TimerIntEnable(TIMERA1_BASE, TIMER_TIMA_TIMEOUT);
   MAP_TimerEnable(TIMERA1_BASE, TIMER_A);

   // Set up LastFM API
   LastFM_Init(LASTFM_API_KEY);
   UART_PRINT("LastFM initialised\n\r");

   query_lastfm();

   // Polling for Remote Inputs
   while (1) {
       /* --- Lyric sync / progress bar ---
        * Read g_ms_tick once to avoid a race if the ISR fires mid-expression.
        * oled_ui_tick() is a no-op when nothing has changed, so calling it
        * on every iteration is safe and cheap. */
       {
           uint32_t now    = g_ms_tick;
           uint32_t dur_ms = (uint32_t)LastFM_GetTrackDurationMs();
           if (dur_ms > 0u) {
               oled_ui_tick(now - g_track_start_ms, dur_ms);
           }
       }

       // A complete IR burst has arrived - decode and dispatch
      if (IR_MessageReady()) {
          int rc5_code = IR_Decode();
          int cmd      = IR_FetchCmd(rc5_code);
          IR_Reset();

          // UART calibration: every decoded command is logged so you can
          // map unknown buttons by pressing them and reading the console.
          UART_PRINT("[IR] cmd=0x%02X (%d)\n\r", (unsigned)cmd, cmd);

          switch (cmd) {

              // ---- Numeric / frequency entry ------------------------------
              // Buttons 0-9: accumulate digits and the decimal point.
              // Button 1 acts as the decimal point
              // The buffer is previewed live on OLED station field.
              case IR_BTN_0:
              case IR_BTN_1:
              case IR_BTN_2:
              case IR_BTN_3:
              case IR_BTN_4:
              case IR_BTN_5:
              case IR_BTN_6:
              case IR_BTN_7:
              case IR_BTN_8:
              case IR_BTN_9:
              case IR_BTN_DOT: {
                  bool accepted = IR_FreqInput_PressDigit(cmd);
                  if (accepted) {
                      // Live-preview: show "> 90.3" style in the station field
                      char preview[UI_MAX_STATION_LEN];
                      snprintf(preview, sizeof(preview), "> %s",
                               IR_FreqInput_GetStr());
                      oled_ui_update_radio(preview, NULL, NULL, NULL, 0, 0);
                      oled_ui_set_view(OLED_VIEW_RADIO);
                      oled_ui_render();
                      UART_PRINT("Freq entry: [%s]\n\r",
                                 IR_FreqInput_GetStr());
                  }
                  break;
              }

              // ---- ENTER: submit frequency --------------------------------
              // SELECT doubles as ENTER when a frequency is being typed.
              // When the buffer is empty it falls back to its old role
              // (switch to the Radio view).
              case IR_BTN_SELECT: {
                  if (IR_FreqInput_IsActive()) {
                      float freq = IR_FreqInput_Submit();  // clears buffer
                      if (freq > 0.0f) {
                          tune_and_update(freq);
                      } else {
                          // Unparseable string (e.g. just ".") - clear and notify
                          UART_PRINT("Invalid freq entry - discarded\n\r");
                          oled_ui_flash_error_banner();
                          char station_str[UI_MAX_STATION_LEN];
                          format_station(g_current_freq, station_str);
                          oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, 0);
                          oled_ui_set_view(OLED_VIEW_RADIO);
                          oled_ui_render();
                      }
                  } else {
                      // Nothing typed - original behavior: jump to Radio view
                      oled_ui_set_view(OLED_VIEW_RADIO);
                      oled_ui_render();
                  }
                  break;
              }
              // ---- View navigation ----------------------------------------
              // After navigating to the Album Cover view, trigger a JPEG
              // fetch if art is already cached from the last LastFM query.
              case IR_BTN_LEFT: {
                  IR_FreqInput_Reset();
                  oled_ui_navigate_left();
                  oled_ui_render();
                  if (oled_ui_get_view() == OLED_VIEW_ALBUM_COVER &&
                          LastFM_AlbumArtAvailable()) {
                      LastFM_RenderAlbumCover();
                  }
                  break;
              }

              case IR_BTN_RIGHT: {
                  IR_FreqInput_Reset();
                  oled_ui_navigate_right();
                  oled_ui_render();
                  if (oled_ui_get_view() == OLED_VIEW_ALBUM_COVER &&
                          LastFM_AlbumArtAvailable()) {
                      LastFM_RenderAlbumCover();
                  }
                  break;
              }

              // ---- Content scrolling (CH/PG +/- buttons) ------------------
              // Calibrate IR_BTN_CH_UP / CH_DOWN via UART if needed.
              case IR_BTN_UP: {
                  oled_ui_scroll_up();
                  oled_ui_render();
                  break;
              }

              case IR_BTN_DOWN: {
                  oled_ui_scroll_down();
                  oled_ui_render();
                  break;
              }

              // ---- Mute / Volume ------------------------------------------
              // TEA5767 has no digital volume register; the PAM8403 handles
              // analog gain via its on-board potentiometer.
              // VOL+ = force unmute, VOL- = force mute, MUTE = toggle.
              case IR_BTN_MUTE: {
                  g_is_muted = !g_is_muted;
                  TEA5767_SetMute(g_is_muted);
                  break;
              }

              // ---- DELETE / Last station ----------------------------------
             // cmd 184 serves two purposes depending on context:
             //   - Freq input active  -> backspace the last typed digit
             //   - Freq input idle    -> recall the last tuned station
             case IR_BTN_LAST: {
                 if (IR_FreqInput_IsActive()) {
                     IR_FreqInput_Delete();
                     if (IR_FreqInput_IsActive()) {
                         char preview[UI_MAX_STATION_LEN];
                         snprintf(preview, sizeof(preview), "> %s",
                                  IR_FreqInput_GetStr());
                         oled_ui_update_radio(preview, NULL, NULL, NULL, 0, 0);
                     } else {
                         char station_str[UI_MAX_STATION_LEN];
                         format_station(g_current_freq, station_str);
                         oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, 0);
                     }
                     oled_ui_set_view(OLED_VIEW_RADIO);
                     oled_ui_render();
                 } else {
                     if (g_last_freq != g_current_freq) {
                         tune_and_update(g_last_freq);
                     }
                 }
                 break;
             }

              // ---- Station seeking (stretch goal) -------------------------
              // TODO: when a dedicated seek button is available, implement:
              //   tune_and_update(g_current_freq + 0.2f);  // CH+
              //   tune_and_update(g_current_freq - 0.2f);  // CH-

              default: {
                  UART_PRINT("Unrecognized code - already been logged\n\r");
                  break;
              }
          }
      }
   }
}
