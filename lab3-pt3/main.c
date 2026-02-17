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
#include <string.h>

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

// Extract current cmd from finished pulse
int fetch_cmd(int rc5_code) {
    if (rc5_code < 0) {
        return -1;
    }
    return rc5_code & 0xFF;
}


/***********************************************************************************
 * Data structures and rules to facilitate multi-tap texting
 */
//**********************************************************************************
// set delay for determining if cycling through characters or printing a character
unsigned char alpha_buttons[254][4] = {
 [252] = {' ', ' ', ' ', '#'},
 [253] = {'.', ',', '!', '#'},
 [248] = {'A', 'B', 'C', '#'},
 [249] = {'D', 'E', 'F', '#'},
 [244] = {'G', 'H', 'I', '#'},
 [240] = {'M', 'N', 'O', '#'},
 [241] = {'P', 'Q', 'R', 'S'},
 [236] = {'T', 'U', 'V', '#'},
 [237] = {'W', 'X', 'Y', 'Z'}
};

//*****************************************************************************
// Multi-tap state and compose / receive buffers
//*****************************************************************************
#define MAX_MSG_LEN  64    // maximum message length (bytes, inc. null)

// Timer A1 callback sets this flag to commit the pending character
volatile bool multitap_timeout = false;

// Active tap state
static int current_tap_cmd = -1;   // cmd code of button being tapped; -1 = none
static int tap_count       =  0;   // number of taps on current_tap_cmd

// Bottom-half compose buffer
static char compose_buf[MAX_MSG_LEN];
static int  compose_len = 0;

// Top-half received-message buffer (written from UART1 ISR)
static char recv_buf[MAX_MSG_LEN];
static int  recv_len = 0;
volatile bool new_message = false;

// Returns the number of valid (non-sentinel) characters for this button.
// Returns 0 if the command code has no text mapping.
static int get_taps_size(int cmd)
{
    if (cmd < 0 || cmd >= 254)         return 0;
    if (alpha_buttons[cmd][0] == 0)    return 0;   // uninitialised entry
    if (cmd == 241 || cmd == 237)      return 4;   // PQRS, WXYZ
    return 3;
}


// fires after 2 seconds have elapsed
void TimerCallback(void) {
    Timer_IF_InterruptClear(TIMERA1_BASE); // Clear the interrupt
    multitap_timeout = true;               // Set boolean
}

//*****************************************************************************
// UART1 – board-to-board (interrupt-driven RX)
//
// Pins must be configured in SysConfig (PinMuxConfig):
//   PIN_07 -> UART1_TX    PIN_08 -> UART1_RX
// Wire: Board-A TX -> Board-B RX, Board-B TX -> Board-A RX, GND <-> GND.
//*****************************************************************************
#define UART1_BAUD_RATE  115200

// ISR staging buffer – builds one line, then copies atomically to recv_buf
static char uart1_stage[MAX_MSG_LEN];
static int  uart1_stage_len = 0;

void UART1_Handler(void)
{
    // Clear all pending UART1 interrupt flags
    unsigned long status = MAP_UARTIntStatus(UARTA1_BASE, true);
    MAP_UARTIntClear(UARTA1_BASE, status);

    // Drain the RX FIFO
    while (MAP_UARTCharsAvail(UARTA1_BASE)) {
        char c = (char)MAP_UARTCharGetNonBlocking(UARTA1_BASE);

        if (c == '\n' || c == '\r') {
            // End of message – publish to main loop
            if (uart1_stage_len > 0) {
                uart1_stage[uart1_stage_len] = '\0';
                memcpy(recv_buf, uart1_stage, (unsigned)uart1_stage_len + 1);
                recv_len           = uart1_stage_len;
                uart1_stage_len    = 0;
                new_message        = true;
            }
        } else if (uart1_stage_len < MAX_MSG_LEN - 1) {
            uart1_stage[uart1_stage_len++] = c;
        }
    }
}

static void UART1_Init(void)
{
    // 1. Clock the UART1 peripheral
    MAP_PRCMPeripheralClkEnable(PRCM_UARTA1, PRCM_RUN_MODE_CLK);
    while (!MAP_PRCMPeripheralStatusGet(PRCM_UARTA1));

    // 2. Configure baud rate and frame format (8-N-1)
    MAP_UARTConfigSetExpClk(UARTA1_BASE,
                            MAP_PRCMPeripheralClockGet(PRCM_UARTA1),
                            UART1_BAUD_RATE,
                            (UART_CONFIG_WLEN_8  |
                             UART_CONFIG_STOP_ONE |
                             UART_CONFIG_PAR_NONE));

    // 3. Register ISR and enable RX + receive-timeout interrupts.
    //    UART_INT_RT fires when the RX FIFO is non-empty and no new
    //    character has arrived for 32 bit-periods – catches short messages
    //    that don't fill the FIFO trigger level.
    MAP_UARTIntRegister(UARTA1_BASE, UART1_Handler);
    MAP_UARTIntEnable(UARTA1_BASE, UART_INT_RX | UART_INT_RT);

    // 4. Enable the UART1 interrupt line in the ARM NVIC
    MAP_IntEnable(INT_UARTA1);

    // 5. Enable UART1
    MAP_UARTEnable(UARTA1_BASE);
}

// Send a null-terminated string followed by '\n' over UART1 (blocking TX)
void UART1_Send(const char *msg)
{
    while (*msg) {
        MAP_UARTCharPut(UARTA1_BASE, (unsigned char)*msg++);
    }
    MAP_UARTCharPut(UARTA1_BASE, '\n');
}

//*****************************************************************************
// OLED display rendering
//
// Characters are drawn with drawChar(x, y, c, color, bg, size=1).
// At size=1 each cell is CHAR_W=6 px wide × CHAR_H=8 px tall.
// 128 / 6 = 21 characters per row.
//*****************************************************************************
#define OLED_WIDTH      128
#define OLED_HALF        64     // y where bottom half starts

#define CHAR_W            6     // character cell width  (5 glyph + 1 gap)
#define CHAR_H            8     // character cell height
#define MAX_CHARS_ROW    21     // characters that fit in one 128-px row

// Row y-coordinates
#define RECV_LABEL_Y      2
#define RECV_MSG_Y       12
#define SEND_LABEL_Y     (OLED_HALF + 2)    // 66
#define SEND_MSG_Y       (OLED_HALF + 12)   // 76

// Convenience: draw a C string at (x,y) with explicit colors
static void draw_text(int x, int y, const char *str,
                      unsigned int color, unsigned int bg,
                      unsigned char size)
{
    while (*str) {
        drawChar(x, y, (unsigned char)*str++, color, bg, size);
        x += CHAR_W * (int)size;
    }
}

// Redraw the entire OLED from global state
void render_display(void)
{
    int i;

    // Top half: received message
    fillRect(0, 0, OLED_WIDTH, (unsigned int)(OLED_HALF - 1), BLACK);
    draw_text(0, RECV_LABEL_Y, "RECV:", CYAN, BLACK, 1);

    if (recv_len > 0) {
        // Scroll so the most recent MAX_CHARS_ROW characters are visible
        int start = (recv_len > MAX_CHARS_ROW) ? recv_len - MAX_CHARS_ROW : 0;
        for (i = start; i < recv_len; i++) {
            drawChar((i - start) * CHAR_W, RECV_MSG_Y,
                     (unsigned char)recv_buf[i], WHITE, BLACK, 1);
        }
    }

    // Divider
    drawFastHLine(0, OLED_HALF - 1, OLED_WIDTH, WHITE);

    // Bottom half: compose area
    fillRect(0, (unsigned int)OLED_HALF, OLED_WIDTH, (unsigned int)OLED_HALF, BLACK);
    draw_text(0, SEND_LABEL_Y, "SEND:", YELLOW, BLACK, 1);

    // Scroll the committed portion of the compose buffer
    int disp_start = (compose_len > MAX_CHARS_ROW) ? compose_len - MAX_CHARS_ROW : 0;
    for (i = disp_start; i < compose_len; i++) {
        drawChar((i - disp_start) * CHAR_W, SEND_MSG_Y,
                 (unsigned char)compose_buf[i], WHITE, BLACK, 1);
    }

    // Render the currently-cycling (uncommitted) character in YELLOW
    if (current_tap_cmd >= 0) {
        int sz = get_taps_size(current_tap_cmd);
        if (sz > 0) {
            char c        = (char)alpha_buttons[current_tap_cmd][tap_count % sz];
            int  disp_pos = compose_len - disp_start;
            if (disp_pos >= 0 && disp_pos < MAX_CHARS_ROW) {
                drawChar(disp_pos * CHAR_W, SEND_MSG_Y,
                         (unsigned char)c, YELLOW, BLACK, 1);
            }
        }
    }
}

//*****************************************************************************
// Multi-tap logic
//*****************************************************************************

// Commit whatever character is currently being cycled into compose_buf,
// stop the timer, and reset tap state.
void commit_current_char(void)
{
    Timer_IF_Stop(TIMERA1_BASE, TIMER_A);

    if (current_tap_cmd >= 0 && compose_len < MAX_MSG_LEN - 1) {
        int  sz = get_taps_size(current_tap_cmd);
        if (sz > 0) {
            char c = (char)alpha_buttons[current_tap_cmd][tap_count % sz];
            compose_buf[compose_len++] = c;
        }
    }

    current_tap_cmd = -1;
    tap_count       =  0;
}

// Restart the 2-second multi-tap commit timer
static void restart_multitap_timer(void)
{
    Timer_IF_Stop(TIMERA1_BASE, TIMER_A);
    Timer_IF_Start(TIMERA1_BASE, TIMER_A, 2000);   // 2 000 ms one-shot
}

// Process one decoded remote button press
static void handle_button(int cmd)
{
    if (cmd < 0) return;

    // MUTE (229): send the composed message
    if (cmd == 229) {
        commit_current_char();
        if (compose_len > 0) {
            compose_buf[compose_len] = '\0';
            UART1_Send(compose_buf);
            Message("SENT: ");
            Message(compose_buf);
            Message("\n\r");
            compose_len = 0;
        }
        render_display();
        return;
    }

    // LAST (184): delete
    if (cmd == 184) {
        Timer_IF_Stop(TIMERA1_BASE, TIMER_A);
        if (current_tap_cmd >= 0) {
            // Cancel the character currently being cycled; do NOT commit it
            current_tap_cmd = -1;
            tap_count       =  0;
        } else if (compose_len > 0) {
            compose_len--;
        }
        render_display();
        return;
    }

    // Text buttons
    if (get_taps_size(cmd) == 0) return;   // unmapped button – ignore

    if (current_tap_cmd == cmd) {
        // Same button tapped again: advance to the next character in the set
        tap_count++;
        restart_multitap_timer();
    } else {
        // Different button: commit whatever was pending, then start fresh
        commit_current_char();
        current_tap_cmd = cmd;
        tap_count       = 0;
        restart_multitap_timer();
    }

    render_display();
}

int main(void)
{
    // Hardware initialization
       BoardInit();
       PinMuxConfig();
       SPIconfig();

       Adafruit_Init();

       // UART0: debug output to host (CCS console)
       InitTerm();
       Message("Board-to-board texting ready\n\r");
       Message("MUTE = SEND   LAST = DELETE\n\r");

       // UART1: board-to-board messaging with interrupt-driven RX
       UART1_Init();

       // SysTick: microsecond pulse timer for IR decoding
       SysTick_Init();

       // GPIOA1 pin 7: IR receiver input (both edges)
       MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1, PRCM_RUN_MODE_CLK);
       while (!MAP_PRCMPeripheralStatusGet(PRCM_GPIOA1));
       MAP_GPIOIntRegister(GPIOA1_BASE, Remote_Handler);
       MAP_GPIOIntTypeSet(GPIOA1_BASE, GPIO_PIN_7, GPIO_BOTH_EDGES);
       MAP_GPIOIntEnable(GPIOA1_BASE, GPIO_PIN_7);

       // Timer A1: one-shot 2-second timer for multi-tap commit
       Timer_IF_Init(PRCM_TIMERA1, TIMERA1_BASE, TIMER_CFG_ONE_SHOT, TIMER_A, 0);
       Timer_IF_IntSetup(TIMERA1_BASE, TIMER_A, TimerCallback);

       fillScreen(WHITE);
       // Draw initial (empty) display
       //render_display();

       while (1)
       {
           // 1. A complete IR burst has arrived – decode and handle it
           if (timer_counter == 1 && pulse_idx > 0) {
               timer_counter = 0;
               int rc5_code = decode_RC5();
               pulse_idx    = 0;
               handle_button(fetch_cmd(rc5_code));
           }

           // 2. Multi-tap timeout fired – commit the pending character
           if (multitap_timeout) {
               multitap_timeout = false;
               commit_current_char();
               render_display();
           }

           // 3. A new message arrived over UART1
           if (new_message) {
               new_message = false;
               Message("RECV: ");
               Message(recv_buf);
               Message("\n\r");
               render_display();
           }
       }
   }
