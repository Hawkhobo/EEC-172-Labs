// Lab 3 Checkoff 2 -- Decoding IR Transmissions / Application Program

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

// Standard include
#include <stdio.h>

// Driverlib includes
#include "hw_types.h"
#include "interrupt.h"
#include "hw_ints.h"
#include "hw_apps_rcm.h"
#include "hw_common_reg.h"
#include "prcm.h"
#include "rom.h"
#include "rom_map.h"
#include "hw_memmap.h"
#include "timer.h"
#include "utils.h"
#include "gpio.h"
#include "systick.h"

// Common interface includes
#include "timer_if.h"
#include "pin_mux_config.h"


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


//***************************************************
//
// Globals used by SysTick and TV Remote Handler
//
//***************************************************
#define RELOAD 0x00FFFFFF // max value for 24 bits
#define TICKS_PER_US 80   // 80MHz clock / 1,000,000
volatile int timer_counter = 0; // current timer_count, labelled volatile so hardware (interrupts) can see it
unsigned long decoded_code = 0;
int bit_count = 0;

// internal registers used by SysTick
#define NVIC_ST_CTRL    (*((volatile unsigned long *)0xE000E010))
#define NVIC_ST_RELOAD  (*((volatile unsigned long *)0xE000E014))
#define NVIC_ST_CURRENT (*((volatile unsigned long *)0xE000E018))

// SysTick control register bitmasks
#define NVIC_ST_CTRL_CLK_SRC  0x00000004
#define NVIC_ST_CTRL_INTEN    0x00000002
#define NVIC_ST_CTRL_ENABLE   0x00000001

// SysTick Configuration: high-speed down-counter with microsecond pulse precision
void SysTick_Init(void) {
    HWREG(NVIC_ST_RELOAD) = RELOAD - 1;
    HWREG(NVIC_ST_CURRENT) = 0; // reset timer
    // enable clock source (processor), interrupt, and the counter
    HWREG(NVIC_ST_CTRL) |= (NVIC_ST_CTRL_CLK_SRC | NVIC_ST_CTRL_INTEN | NVIC_ST_CTRL_ENABLE);
}


void SysTick_Handler() {
    // set timer_counter to 1 if we haven't seen an IR edge in 209ms (message concluded)
    timer_counter = 1;

    // Clear count and interrupt flag; give control back to software
    HWREG(NVIC_ST_CURRENT) = 0;
}

// primary logic for interrupt handling and parsing Remote signals
void Remote_Handler() {
    // clear interrupt flag for pin 06
    unsigned long status = MAP_GPIOIntStatus(GPIOA0_BASE, true);
    MAP_GPIOIntClear(GPIOA0_BASE, status);

    // Measure elapsed time since last edge
    // regisers NVIC_ST_CURRENT stores current value of SysTick
    int time_ticks = RELOAD - HWREG(NVIC_ST_CURRENT);

    // reset for next pulse
    HWREG(NVIC_ST_CURRENT) = 0;

    // Check watchdog flag, reset state if transmission is timed out
    if (timer_counter == 1) {
        bit_count = 0;
        decoded_code = 0;
        timer_counter = 0;
        return;
    }

    // Read pin 06 and determine pulse type
    int pin_val = MAP_GPIOPinRead(GPIOA0_BASE, GPIO_PIN_6);
    int time_us = time_ticks / TICKS_PER_US; // convert to microseconds

    // Logic for RC-5/RC-6 protocol
    // Look for short 889us and long 1778us pulses
    if (time_us > 1500 && time_us < 2000) {
        printf("Detected Long Pulse: %d us\n", time_us);
    } else if (time_us > 700 && time_us < 1100) {
        printf("Detected Short Pulse: %d us\n", time_us);
    }
}


int main(void)
{
    //
    // Initialize board configurations
    BoardInit();
    //
    // Pinmuxing for LEDs
    //
    PinMuxConfig();

    SysTick_Init();

    // Configure Pin 06 on rising and falling edges
    MAP_GPIOIntRegister(GPIOA0_BASE, Remote_Handler);
    MAP_GPIOIntTypeSet(GPIOA0_BASE, GPIO_PIN_6, GPIO_BOTH_EDGES);
    MAP_GPIOIntEnable(GPIOA0_BASE, GPIO_PIN_6);

    //
    // Loop forever while the timers run.
    //
    while(1)
    {
    }
}
