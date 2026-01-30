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
#include <stdint.h>

// Driverlib includes
#include "hw_types.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "hw_ints.h"
#include "i2c_if.h"
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
#define DRAW_DELAY 24e6
#define WHITE 0xFFFF
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

    // enable internal SPI CS to satisfy SPI API; using GPIOP CS instead
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

    // registers for Accelerometer values
    unsigned char reg_x = 0x03, reg_y = 0x05;
    // raw x and y values should be int8_t to respect 2's complement (include negatives)
    int8_t x_raw, y_raw;

    float speed = 0.1;

    // Initial position: center of screen
    int x = SSD1351WIDTH / 2, y = SSD1351HEIGHT / 2;
    // Radius should be about 4 pixels
    int radius = 2;
    // Run accelerometer circle on Adafruit OLED in a loop
    fillScreen(WHITE);
    fillCircle(x, y, radius, RED);
    while(1)
    {
        // refresh x,y variables based on accelerometer (read registers, write to var)
        I2C_IF_Write(0x18, &reg_x, 1, 0);
        I2C_IF_Read(0x18, (unsigned char *)&x_raw, 1);

        I2C_IF_Write(0x18, &reg_y, 1, 0);
        I2C_IF_Read(0x18, (unsigned char *)&y_raw, 1);

        // Update position
        int next_x = x + (int)(x_raw * speed);
        int next_y = y + (int)(y_raw * speed);

        // Bound to screen edges
        if(next_x < radius) next_x = radius;
        if(next_x > SSD1351WIDTH - radius) next_x = SSD1351WIDTH - radius;
        if(next_y < radius) next_y = radius;
        if(next_y > SSD1351HEIGHT - radius) next_y = SSD1351HEIGHT - radius;

        // redraw if ball needs to move
        if (next_x != x || next_y != y) {
            // erase old ball
            fillCircle(x, y, radius, WHITE);

            // Update to new coordinates
            x = next_x;
            y = next_y;

            // new ball
            fillCircle(x, y, radius, RED);
        }

        UtilsDelay(80000);

    }

}
