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
// Application Name     - CC3200 GPIO Application
// Application Overview - Push SW3 switch to start LED binary counting.
//                        Or, push SW2 switch to blink LEDs on & off.
//                        Code adapted from ti CC3200 SDK blinky example,
//                        all licenses apply.
//
//*****************************************************************************

//****************************************************************************
//
//! \addtogroup blinky
//! @{
//
//****************************************************************************

// Standard includes
#include <stdio.h>

// Driverlib includes
#include "hw_types.h"
#include "hw_ints.h"
#include "hw_memmap.h"
#include "hw_common_reg.h"
#include "interrupt.h"
#include "hw_apps_rcm.h"
#include "prcm.h"
#include <stdbool.h>
#include "rom.h"
#include "rom_map.h"
#include "prcm.h"
#include "gpio.h"
#include "uart.h"
#include "uart_if.h"
#include "utils.h"

// Common interface includes
#include "gpio_if.h"

#include "pin_mux_config.h"


//*****************************************************************************
//                          MACROS                                  
//*****************************************************************************
#define APPLICATION_VERSION     "1.4.0"
// the amount of time used for delay in CC3200 SDK'S blinky example
#define BLINKY_TIME 8000000
#define SCALAR_MULTIPLE 3

//*****************************************************************************
//                 GLOBAL VARIABLES -- Start
//*****************************************************************************
#if defined(ccs)
extern void (* const g_pfnVectors[])(void);
#endif
#if defined(ewarm)
extern uVectorEntry __vector_table;
#endif
//*****************************************************************************
//                 GLOBAL VARIABLES -- End
//*****************************************************************************


//*****************************************************************************
//                      LOCAL FUNCTION PROTOTYPES                           
//*****************************************************************************
void SW3_BINARY_LED_ROUTINE();
void SW2_UNISON_LED_ROUTINE();
static void BoardInit(void);
static void DisplayHeader(void);

//*****************************************************************************
//                      LOCAL FUNCTION DEFINITIONS                         
//*****************************************************************************

//*****************************************************************************
//
//! Configures the pins as GPIOs and periodically toggles the lines
//!
//! \param None
//! 
//! This function  
//!    1. Configures 3 lines connected to LEDs as GPIO
//!    2. Sets up the GPIO pins as output
//!    3. Toggles LED's in a binary sequence, 000 -> 111, with delay in-between. When 111 is reached, it loops back to the start.
//!
//! \return None
//
//*****************************************************************************
void SW3_BINARY_LED_ROUTINE()
{
    //
    // Toggle the lines initially to turn off the LEDs.
    // The values driven are as required by the LEDs on the LP.
    //
    GPIO_IF_LedOff(MCU_ALL_LED_IND);
    Message("SW3 pressed");

    // set P18 signal to low
    GPIOPinWrite(GPIOA3_BASE, GPIO_PIN_4, 0);

    // use a 3-bit for loop strategy to toggle the appropriate LEDs
    unsigned int i;
    for (i = 0; i < 8; i++)
    {
        // MSB (corresponds to green)
        if (i & (1 << 2))
        {
            GPIO_IF_LedOn(MCU_GREEN_LED_GPIO);
        }
        else
        {
            GPIO_IF_LedOff(MCU_GREEN_LED_GPIO);
        }

        // middle bit (corresponds to orange)
        if (i & (1 << 1))
        {
            GPIO_IF_LedOn(MCU_ORANGE_LED_GPIO);
        }
        else
        {
            GPIO_IF_LedOff(MCU_ORANGE_LED_GPIO);
        }


        // LSB (corresponds to red)
        if (i & (1 << 0))
        {
            GPIO_IF_LedOn(MCU_RED_LED_GPIO);
        }
        else
        {
            GPIO_IF_LedOff(MCU_RED_LED_GPIO);
        }

        // Delay for a time, then toggle off
        MAP_UtilsDelay(SCALAR_MULTIPLE * BLINKY_TIME);
        GPIO_IF_LedOff(MCU_ALL_LED_IND);
    }
}

//*****************************************************************************
//
//! Configures the pins as GPIOs and periodically toggles the lines
//!
//! \param None
//!
//! This function
//!    1. Configures 3 lines connected to LEDs as GPIO
//!    2. Sets up the GPIO pins as output
//!    3. Toggles the LED's in unison, with momentary delay in between.
//!
//! \return None
//
//*****************************************************************************
void SW2_UNISON_LED_ROUTINE()
{
    GPIO_IF_LedOff(MCU_ALL_LED_IND);
    Message("SW2 pressed");

    // set P18 signal to high (bitmask must be matched to set to high)
    GPIOPinWrite(GPIOA3_BASE, GPIO_PIN_4, GPIO_PIN_4);

    // let on-and-off unison pattern take place 3 times
    unsigned int i;
    for (i = 0; i < 3; i++)
    {
        GPIO_IF_LedOn(MCU_ALL_LED_IND);
        MAP_UtilsDelay(SCALAR_MULTIPLE * BLINKY_TIME);
        GPIO_IF_LedOff(MCU_ALL_LED_IND);
        MAP_UtilsDelay(SCALAR_MULTIPLE * BLINKY_TIME);
    }
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
//! Display header and usage instructions to console when program starts
//!
//! \param  None
//!
//! \return None
//
//*****************************************************************************
static void
DisplayHeader(void)
{
    Message("****************************************************\n\r");
    Message("                                                    \n\r");
    Message("\tCC3200 GPIO Application                           \n\r");
    Message("                                                    \n\r");
    Message("****************************************************\n\r");
    Message("                                                    \n\r");
    Message("****************************************************\n\r");
    Message("                                                    \n\r");
    Message("\tPush SW3 to start LED binary counting             \n\r");
    Message("                                                    \n\r");
    Message("\tPush SW2 to blink LEDs on and off                 \n\r");
    Message("                                                    \n\r");
    Message("****************************************************\n\n\r");
}

//****************************************************************************
//
//! Main function
//!
//! \param none
//! 
//! This function  
//!    1. Invoked the SW3 binary task if SW3 is pressed
//!    2. Invokes the SW2 on-off unison task if SW2 is pressed
//!
//! \return None.
//
//****************************************************************************
int
main()
{
    //
    // Initialize Board configurations
    //
    BoardInit();

    // enable pin multiplexers defined in .sysconf
    PinMuxConfig();

    // terminal header text
    InitTerm();
    ClearTerm();
    DisplayHeader();

    
    //
    // Power on the corresponding GPIO port B for 9,10,11.
    // Set up the GPIO lines to mode 0 (GPIO)
    //
    GPIO_IF_LedConfigure(LED1|LED2|LED3);
    GPIO_IF_LedOff(MCU_ALL_LED_IND);
    

    //
    // Listen for switch presses (waste clock cycles, but simple implementation)
    //
    bool use_sw3 = 0;
    bool use_sw2 = 0;
    while(1) {
        // Value of SW3 (GPIO_13 is bit (pin) 5, port group A1)
        // If pin 5's bit-value is 1, SW3 was pressed
        if ((GPIOPinRead(GPIOA1_BASE, GPIO_PIN_5) & GPIO_PIN_5) && !use_sw2)
        {
            use_sw3 = 0;
            use_sw2 = 1;
            SW3_BINARY_LED_ROUTINE();
            Message("\n");
        }
        // Otherwise, check value of SW2 (GPIO_22 is bit (pin) 6, port group A2)
        // if pin 6's bit-value is 1, SW2 was pressed
        if ((GPIOPinRead(GPIOA2_BASE, GPIO_PIN_6) & GPIO_PIN_6) && !use_sw3)
        {
           use_sw3 = 1;
           use_sw2 = 0;
           SW2_UNISON_LED_ROUTINE();
           Message("\n");
        }
    }

    return 0;
}

//*****************************************************************************
//
// Close the Doxygen group.
//! @}
//
//*****************************************************************************
