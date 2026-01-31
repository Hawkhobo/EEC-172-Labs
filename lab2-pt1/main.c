//*****************************************************************************
// Lab 2 - Checkoff 1 & 2
// Jacob Feenstra and Chun Ho Chen
//*****************************************************************************
//
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

//*****************************************************************************
//
// Application Name     - Lab 2 Pt. 1 -- Adafruit SPI
// Application Overview - Enable SPI communication between the CC3200 microcontroller
//                        and the Adafruit SSD1351 OLED
//
//*****************************************************************************


//*****************************************************************************
//
//! \addtogroup SPI_Demo
//! @{
//
//*****************************************************************************

// Standard includes
#include <string.h>
#include <stdbool.h>

// Driverlib includes
#include "hw_types.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "hw_ints.h"
#include "spi.h"
#include "rom.h"
#include "rom_map.h"
#include "utils.h"
#include "prcm.h"
#include "uart.h"
#include "interrupt.h"

// Common interface includes
#include "pin_mux_config.h"

// OLED Adafruit functions for output via SPI
#include "adafruit_oled_lib/Adafruit_SSD1351.h"
#include "adafruit_oled_lib/Adafruit_GFX.h"
#include "adafruit_oled_lib/glcdfont.h"
#include "adafruit_oled_lib/oled_test.h"


#define APPLICATION_VERSION     "1.4.0"

#define SPI_IF_BIT_RATE  1000000
#define TR_BUFF_SIZE     100

#define MASTER_MSG       "This is CC3200 SPI Master Application\n\r"

//*****************************************************************************
//                 GLOBAL VARIABLES -- Start
//*****************************************************************************

#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif

#define GLCD_FONT_SIZE 1270
// delay sweet spot: about 2 seconds
#define DRAW_DELAY 24e6
#define WHITE 0xFFFF
// pixel width for spacing of ASCII characters on Adafruit display
#define PIXEL_WIDTH 8
//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************


//*****************************************************************************
//
//!
//!
//! Configure SPI for communication
//!
//! \return None.
//
//*****************************************************************************
void SPIconfig()
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
//! Main function for SPI OLED application
//!
//! \param none
//!
//! \return None.
//
//*****************************************************************************
void main()
{
    //
    // Initialize Board configurations
    //
    BoardInit();

    //
    // Muxing SPI, GPIOP lines.
    //
    PinMuxConfig();

    //
    // I2C Init
    //
    I2C_IF_Open(I2C_MASTER_MODE_FST);


    // Configure SPI for configuration
    SPIconfig();


    // Initialize OLED display
    Adafruit_Init();

    fillScreen(WHITE);
    // Run Adafruit output in a loop
    while(1)
    {
        // Print full character set
        int i = 0; int x = 0; int y = 0;
        bool finished = false;
        // NOTE: adjust increment count as needed if fonts are too closely packed
        for (x = 0; x < SSD1351WIDTH && !finished; x += PIXEL_WIDTH) {
            for (y = 0; y < SSD1351HEIGHT; y += PIXEL_WIDTH) {
                if (i < GLCD_FONT_SIZE) {
                    drawChar(x, y, font[i++], 1, 1, 1);
                }
                else {
                    finished = true;
                    break;
                }

            }
        }

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        // Print hello world
        unsigned char greeting[] = "Hello world!";
        i = 0; x = 0; y = 0;
        while (greeting[i] != '\0') {
            drawChar(x, y, greeting[i], 1, 1, 1);
            x += PIXEL_WIDTH;
            i++;
        }

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        // 8 bands of different colors horizontally across OLED display
        unsigned int colors[] = {0xfc0303, 0xfc6c05, 0xfcc205, 0xcffc05, 0x05fca1, 0x0509fc, 0x8105fc, 0xfc05f4};
        int num_bands = 8;

        i = 0; x = 0; y = 0;
        int width = SSD1351WIDTH / num_bands;
        for (y = 0; y < SSD1351WIDTH; y += width) {
                   drawFastHLine(x, y, SSD1351WIDTH, colors[i]);
                   i++;
        }


        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        // And now vertically
        i = 0; x = 0; y = 0;
        int height = SSD1351HEIGHT / num_bands;
        for (x = 0; x < SSD1351HEIGHT; x += height)
        {
            drawFastVLine(x, y, SSD1351HEIGHT, colors[i]);
            i++;
        }

        // Run requested Adafruit API printout fucntions. See Adafruit_OLED.c for details on each

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);


        testlines(colors[0]);

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testfastlines(colors[0], colors[1]);

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testdrawrects(colors[2]);

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testfillrects(colors[3], colors[4]);

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testfillcircles(32, colors[4]);

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testdrawcircles(32, colors[5]);

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testroundrects();

        UtilsDelay(DRAW_DELAY);
        fillScreen(WHITE);

        testtriangles();

        fillScreen(WHITE);
    }

}
