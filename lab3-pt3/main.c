// Lab 3 Checkoff 3 -- Board to Board Texting via UART
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

// Standard include
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

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
#include "Debug/syscfg/pin_mux_config.h"
#include "timer_if.h"
#include "uart_if.h"

// OLED Adafruit functions for output via SPI
#include "adafruit_oled_lib/Adafruit_SSD1351.h"
#include "adafruit_oled_lib/Adafruit_GFX.h"
#include "adafruit_oled_lib/glcdfont.h"
#include "adafruit_oled_lib/oled_test.h"


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
// Globals for SPI communication
//*****************************************************************************
#define SPI_IF_BIT_RATE  1000000
#define TR_BUFF_SIZE     100

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


//***************************************************
//
// Globals used by SysTick and TV Remote Handler
//
//***************************************************
#define RELOAD 0x00FFFFFF // max value for 24 bits
#define TICKS_PER_US 80   // 80MHz clock / 1,000,000
void SysTick_Handler(void);


// internal registers used by SysTick
#define NVIC_ST_CTRL    0xE000E010
#define NVIC_ST_RELOAD  0xE000E014
#define NVIC_ST_CURRENT 0xE000E018

// SysTick control register bitmasks
#define NVIC_ST_CTRL_CLK_SRC  0x00000004
#define NVIC_ST_CTRL_INTEN    0x00000002
#define NVIC_ST_CTRL_ENABLE   0x00000001

// RC-5 protocol constants
#define T_UNIT       889
#define T_SHORT_MAX  1334
#define T_NOISE_MIN  300
#define T_LONG_MAX   2500

// raw edge timings
volatile uint32_t pulse_buffer[128]; // Buffer to store timings
volatile uint32_t pulse_idx = 0;

// cross-ISR flag
volatile int timer_counter = 0; // current timer_count, labelled volatile so hardware (interrupts) can see it

// SysTick Configuration: high-speed down-counter with microsecond pulse precision
void SysTick_Init(void) {
    SysTickIntRegister(SysTick_Handler);

    HWREG(NVIC_ST_RELOAD) = RELOAD - 1;
    HWREG(NVIC_ST_CURRENT) = 0; // reset timer
    // enable clock source (processor), interrupt, and the counter
    HWREG(NVIC_ST_CTRL) |= (NVIC_ST_CTRL_CLK_SRC |
                            NVIC_ST_CTRL_INTEN   |
                            NVIC_ST_CTRL_ENABLE  );
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
    unsigned long status = MAP_GPIOIntStatus(GPIOA1_BASE, true);
    MAP_GPIOIntClear(GPIOA1_BASE, status);

    // Measure elapsed time since last edge
    // regisers NVIC_ST_CURRENT stores current value of SysTick
    uint32_t time_ticks = RELOAD - HWREG(NVIC_ST_CURRENT);

    // reset for next pulse
    HWREG(NVIC_ST_CURRENT) = 0;

    // Logic for RC-5/RC-6 protocol
    // Look for short 889us and long 1778us pulses
    // Filter noise, but capture everything else
    if (timer_counter == 0) {
            if (pulse_idx < 128) {
                int pin_val = MAP_GPIOPinRead(GPIOA1_BASE, GPIO_PIN_7);
                uint32_t time_us = time_ticks / TICKS_PER_US;

                // Store polarity in MSB: 1 = Pulse was HIGH, 0 = Pulse was LOW
                // Note: If pin is LOW now, the pulse that just ended was HIGH.
                if (pin_val == 0) {
                    pulse_buffer[pulse_idx] = time_us | 0x80000000u;
                } else {
                    pulse_buffer[pulse_idx] = time_us;
                }
                pulse_idx++;
            }
        } else {
            // New transmission starting
            timer_counter = 0;
            pulse_idx = 0;
        }
}

static int decode_RC5(void) {
    if (pulse_idx < 10) return -1;  // Too few edges to be a valid RC-5 frame

    uint32_t code = 0;
    int bits     = 0;
    int half_waiting = 0;

    int i;
    for (i = 0; i < (int)pulse_idx && bits < 14; i++) {
        uint32_t t    = pulse_buffer[i] & 0x7FFFFFFFu;
        int      high = (pulse_buffer[i] & 0x80000000u) ? 1 : 0;
        // high == 1    the pulse that ended was HIGH (no carrier)
        // high == 0  the pulse that ended was LOW  (carrier active)

        // Discard noise and inter-frame gaps
        if (t < T_NOISE_MIN || t > T_LONG_MAX) continue;

        int is_short = (t < T_SHORT_MAX);

        if (is_short) {
            // A short pulse is one half-bit interval.
            if (!half_waiting) {
                // This is the FIRST half of a new bit; save and wait.
                half_waiting = 1;
            } else {
                // This is the SECOND half; the bit value equals the level
                // of this second half.
                //   RC-5 "1" second half HIGH high == 1, bit = 1
                //   RC-5 "0" second half LOW high == 0, bit = 0
                code = (code << 1) | (unsigned int)high;
                bits++;
                half_waiting = 0;
            }
        } else {
            // A long pulse spans TWO half-bit intervals at the same level.
            // This always completes the currently pending half-bit AND starts
            // the next bit's first half (which is the same level).
            if (!half_waiting) {
                // Long pulse at a full-bit boundary (unusual but can happen
                // at the very start of the frame). Treat as first-half only.
                half_waiting = 1;
            } else {
                // Complete the pending bit: second half = this pulse's level.
                code = (code << 1) | (unsigned int)high;
                bits++;
                // The end of this long pulse is also the first half of the
                // NEXT bit (same level), so remain in half_waiting = 1.
                half_waiting = 1;
            }
        }
    }

    if (bits < 14) return -1;   // incomplete frame
    return (int)(code & 0x3FFFu);
}

static void print_button(int rc5_code) {
    if (rc5_code < 0) {
        return;
    }


    int cmd  = rc5_code & 0xFF;         // 8-bit command
    int addr = (rc5_code >> 6) & 0x1F;  // 5-bit address (device type)

    char buf[48];

    // Standard Philips RC-5 TV command codes (address = 0).
    // "Last" (previous channel) = 0x12 = 18 on most RC-5 remotes.
    // "Enter" / "OK" / "Zoom"   = 0x0D = 13; adjust if your remote differs.
    switch (cmd) {
        case 252:  Message("Button: 0\n\r");          break;
        case 253:  Message("Button: 1\n\r");          break;
        case 248:  Message("Button: 2\n\r");          break;
        case 249:  Message("Button: 3\n\r");          break;
        case 244:  Message("Button: 4\n\r");          break;
        case 245:  Message("Button: 5\n\r");          break;
        case 240:  Message("Button: 6\n\r");          break;
        case 241:  Message("Button: 7\n\r");          break;
        case 236:  Message("Button: 8\n\r");          break;
        case 237:  Message("Button: 9\n\r");          break;
        case 229: Message("Button: MUTE\n\r");       break;  // 0x12 prev-channel
        case 184: Message("Button: LAST\n\r"); break;  // 0x0D select/OK
        default:
            // Unknown command: print raw values so you can calibrate the table.
            sprintf(buf, "Unknown: addr=%d cmd=%d (raw=0x%04X)\n\r",
                    addr, cmd, rc5_code);
            Message(buf);
            break;
    }
}

/***********************************************************************************
 * Globals used by printing to OLED
 */
//**********************************************************************************
#define ASCII_WIDTH 5
#define ASCII_HEIGHT 7

/***********************************************************************************
 * Data structures and rules to facilitate multi-tap texting
 */
//**********************************************************************************
// set delay for determining if cycling through characters or printing a character

#define DELAY 2e6 // 2s in us

struct ascii_buttons {
  unsigned char b_0[1];
  unsigned char b_2[3];
  unsigned char b_3[3];
  unsigned char b_4[3];
  unsigned char b_5[3];
  unsigned char b_6[3];
  unsigned char b_7[4];
  unsigned char b_8[3];
  unsigned char b_9[4];
};

struct ascii_buttons alpha = {
 {' '},
 {'A', 'B', 'C'},
 {'D', 'E', 'F'},
 {'G', 'H', 'I'},
 {'J', 'K', 'L'},
 {'M', 'N', 'O'},
 {'P', 'Q', 'R', 'S'},
 {'T', 'U', 'V'},
 {'W', 'X', 'Y', 'Z'}
};

// Button order: {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, Mute, Last }
int button_cmds[12] = {252, 253, 248, 249, 244, 245, 240, 241, 236, 237, 229, 184};

// Extract current cmd from finished pulse
int fetch_cmd(int rc5_code) {
    if (rc5_code < 0) {
        return;
    }
    return rc5_code & 0xFF;
}


// fires after 2 seconds have elapsed
volatile bool multitap_timeout = false;
void TimerCallback(void) {
    Timer_IF_InterruptClear(TIMERA1_BASE); // Clear the interrupt
    multitap_timeout = true;               // Set boolean
}



// Given a button press, print the selected character
static void print_OLED(int rc5_code) {
    if (rc5_code < 0) {
          return;
      }

    int cmd  = rc5_code & 0xFF; // 8-bit command

    fillScreen(WHITE);
    int x = 0, y = 0;
    switch (cmd) {
        case 252:  drawChar(x, y, '0', 1, 1, 1); break;
        case 253:  drawChar(x, y, '1', 1, 1, 1); break;
        case 248:  drawChar(x, y, '2', 1, 1, 1); break;
        case 249:  drawChar(x, y, '3', 1, 1, 1); break;
        case 244:  drawChar(x, y, '4', 1, 1, 1); break;
        case 245:  drawChar(x, y, '5', 1, 1, 1); break;
        case 240:  drawChar(x, y, '6', 1, 1, 1); break;
        case 241:  drawChar(x, y, '7', 1, 1, 1); break;
        case 236:  drawChar(x, y, '8', 1, 1, 1); break;
        case 237:  drawChar(x, y, '9', 1, 1, 1); break;
        case 229:  drawChar(x, y, 'M', 1, 1, 1); break;
        case 184:  drawChar(x, y, 'L', 1, 1, 1); break;
        default:   break;
    }

}



int main(void)
{
    //
    // Initialize board configurations
    BoardInit();
    //
    // Pinmuxing for IR receiver and UART
    //
    PinMuxConfig();

    // Configure SPI for OLED
    SPIconfig();
    // Initialize and turn on OLED
    Adafruit_Init();
    fillScreen(WHITE);

    // init UART terminal
    InitTerm();

    SysTick_Init();



    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1, PRCM_RUN_MODE_CLK);
    while(!MAP_PRCMPeripheralStatusGet(PRCM_GPIOA1));

    // Configure physical Pin 06 on rising and falling edges
    MAP_GPIOIntRegister(GPIOA1_BASE, Remote_Handler);
    MAP_GPIOIntTypeSet(GPIOA1_BASE, GPIO_PIN_7, GPIO_BOTH_EDGES);
    MAP_GPIOIntEnable(GPIOA1_BASE, GPIO_PIN_7);

    // Initialize Timer A1 (different from the IR timer) for 2-second counter
    // Used in multi-tap text functionality
    Timer_IF_Init(PRCM_TIMERA1, TIMERA1_BASE, TIMER_CFG_ONE_SHOT, TIMER_A, 0);
    Timer_IF_IntSetup(TIMERA1_BASE, TIMER_A, TimerCallback);


    //
    // Loop forever while the timers run.
    //
    Message("start looping");
    while(1)
    {
        // If the watchdog fired, it means the burst is finished
        if (timer_counter == 1 && pulse_idx > 0) {
            timer_counter = 0;

            multitap_timeout = false;
            Timer_IF_Start(TIMERA1_BASE, TIMER_A, 2000);
            // multitap_timer runs for 2 seconds
            int rc5_code = -1;
            int cmd = 0;
            while (!multitap_timeout) {
                rc5_code = decode_RC5();
                cmd = fetch_cmd(rc5_code);


            }
            rc5_code = decode_RC5();
            print_button(rc5_code);
            //print_OLED(rc5_code);
            pulse_idx = 0; // Reset for next button press
        }
    }
}
