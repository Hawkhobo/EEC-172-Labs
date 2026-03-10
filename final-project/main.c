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
#include "TSOP311_IR_RECEIVER/tsop311_ir_receiver.h"


#define DATE                9    /* Current Date */
#define MONTH               3     /* Month 1-12 */
#define YEAR                2026  /* Current year */
#define HOUR                21    /* Time - hours */
#define MINUTE              20  /* Time - minutes */
#define SECOND              0     /* Time - seconds */

#define APPLICATION_NAME      "SSL"
#define APPLICATION_VERSION   "WQ26"
#define SERVER_NAME           "a3i55f5e4s85vq-ats.iot.us-east-1.amazonaws.com"
#define GOOGLE_DST_PORT       8443

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
#define SPI_IF_BIT_RATE  1000000
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


//*****************************************************************************
//
//! This function updates the date and time of CC3200.
//!
//! \param None
//!
//! \return
//!     0 for success, negative otherwise
//!
//*****************************************************************************
static int set_time() {
    long retVal;

    g_time.tm_day = DATE;
    g_time.tm_mon = MONTH;
    g_time.tm_year = YEAR;
    g_time.tm_sec = HOUR;
    g_time.tm_hour = MINUTE;
    g_time.tm_min = SECOND;

    retVal = sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION,
                          SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME,
                          sizeof(SlDateTime),
                          (unsigned char *)(&g_time));

    ASSERT_ON_ERROR(retVal);
    return SUCCESS;
}

static float g_current_freq = 90.3f;   // Initialised to boot frequency
static float g_last_freq    = 90.3f;   // Previous station (same as current at boot)

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
static int tune_and_update(float freq_mhz)
{
    int rc = TEA5767_TuneFrequency(freq_mhz);

    char station_str[UI_MAX_STATION_LEN];

    if (rc == TEA5767_OK) {
        g_last_freq    = g_current_freq;
        g_current_freq = freq_mhz;
        format_station(g_current_freq, station_str);
        UART_PRINT("Tuned to %.1f FM\n\r", (double)g_current_freq);
    } else {
        // Keep displaying the current (still-tuned) station; append an error hint.
        format_station(g_current_freq, station_str);
        UART_PRINT("Tune failed (rc=%d) for %.1f\n\r", rc, (double)freq_mhz);
    }

    oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, 0);
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
   oled_ui_update_radio("90.3 FM", "", "", "", 0, 0);
   oled_ui_render();

   // IR Receiver
   IR_Init();

   // Network: Wi-Fi -> TLS -> LastFM API
   g_app_config.host = SERVER_NAME;
   g_app_config.port = GOOGLE_DST_PORT;

   long lRetVal = connectToAccessPoint();
   if (lRetVal < 0) {
       UART_PRINT("Wi-Fi connection failed\n\r");
       LOOP_FOREVER();
   }

   lRetVal = set_time();
   if (lRetVal < 0) {
       UART_PRINT("set_time() failed — TLS will not work\n\r");
       LOOP_FOREVER();
   }

   lRetVal = tls_connect();
   if (lRetVal < 0) {
       UART_PRINT("TLS connect failed: %d\n\r", (int)lRetVal);
       // Non-fatal for now; LastFM calls will fail gracefully
   }

   UART_PRINT("Network Ready\n\r");

   // Polling for Remote Inputs
   while (1) {
       // A complete IR burst has arrived - decode and dispatch
      if (IR_MessageReady()) {
          int rc5_code = IR_Decode();
          int cmd      = IR_FetchCmd(rc5_code);
          IR_Reset();

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
              case IR_BTN_9: {
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
                          char station_str[UI_MAX_STATION_LEN];
                          format_station(g_current_freq, station_str);
                          oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, 0);
                          oled_ui_set_view(OLED_VIEW_RADIO);
                          oled_ui_render();
                      }
                  } else {
                      // Nothing typed - original behaviour: jump to Radio view
                      oled_ui_set_view(OLED_VIEW_RADIO);
                      oled_ui_render();
                  }
                  break;
              }

              // ---- DELETE: backspace the last entered character -----------
              case IR_BTN_BACK: {
                  IR_FreqInput_Delete();
                  if (IR_FreqInput_IsActive()) {
                      char preview[UI_MAX_STATION_LEN];
                      snprintf(preview, sizeof(preview), "> %s",
                               IR_FreqInput_GetStr());
                      oled_ui_update_radio(preview, NULL, NULL, NULL, 0, 0);
                  } else {
                      // Buffer now empty - restore current station label
                      char station_str[UI_MAX_STATION_LEN];
                      format_station(g_current_freq, station_str);
                      oled_ui_update_radio(station_str, NULL, NULL, NULL, 0, 0);
                  }
                  oled_ui_set_view(OLED_VIEW_RADIO);
                  oled_ui_render();
                  break;
              }

              // ---- View navigation ----------------------------------------
              case IR_BTN_LEFT:
                  IR_FreqInput_Reset();   // Cancel any pending entry on nav
                  oled_ui_navigate_left();
                  oled_ui_render();
                  break;

              case IR_BTN_RIGHT:
                  IR_FreqInput_Reset();   // Cancel any pending entry on nav
                  oled_ui_navigate_right();
                  oled_ui_render();
                  break;

              // ---- Content scrolling --------------------------------------
              case IR_BTN_VOL_UP:
                  oled_ui_scroll_up();
                  oled_ui_render();
                  break;

              case IR_BTN_VOL_DOWN:
                  oled_ui_scroll_down();
                  oled_ui_render();
                  break;

              // ---- Station seeking ----------------------------------------
              case IR_BTN_SEEK_UP:
                  // TODO: increment frequency by 0.2 MHz (one US FM channel step)
                  // tune_and_update(g_current_freq + 0.2f);
                  break;

              case IR_BTN_SEEK_DOWN:
                  // TODO: decrement frequency by 0.2 MHz
                  // tune_and_update(g_current_freq - 0.2f);
                  break;

              // ---- Mute toggle --------------------------------------------
              case IR_BTN_MUTE:
                  // TODO: TEA5767_SetMute(toggle)
                  break;

              default:
                  UART_PRINT("Unknown IR cmd: %d\n\r", cmd);
                  break;
          }
      }
  }
}
