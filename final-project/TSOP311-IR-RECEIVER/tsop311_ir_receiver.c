// IR Receiver Driver — RC-5 Protocol Decoder
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// See ir_receiver.h for full documentation

#include "tsop311_ir_receiver.h"

#include <stdint.h>
#include <stdbool.h>

// Driverlib includes
#include "gpio.h"
#include "hw_types.h"
#include "hw_ints.h"
#include "hw_memmap.h"
#include "interrupt.h"
#include "prcm.h"
#include "rom_map.h"
#include "systick.h"

//*****************************************************************************
//                      SysTick Register Addresses & Bitmasks
//*****************************************************************************
#define NVIC_ST_CTRL    0xE000E010
#define NVIC_ST_RELOAD  0xE000E014
#define NVIC_ST_CURRENT 0xE000E018

#define NVIC_ST_CTRL_CLK_SRC    0x00000004  // Use processor clock
#define NVIC_ST_CTRL_INTEN      0x00000002  // Enable SysTick interrupt
#define NVIC_ST_CTRL_ENABLE     0x00000001  // Enable counter

// 24-bit reload value, wraps every ~209 ms at 80 MHz.
// Overflow means transmission has ended.
#define IR_SYSTICK_RELOAD   0x00FFFFFF

// Convert SysTick ticks to microseconds (80 MHz clock, 80 ticks/microsecond)
#define IR_TICKS_PER_US     80

//*****************************************************************************
//                      Shared Globals
//*****************************************************************************
volatile uint32_t ir_pulse_buffer[IR_PULSE_BUF_SIZE];
volatile uint32_t ir_pulse_idx    = 0;
volatile int      ir_timer_counter = 0;

//*****************************************************************************
//                      SysTick ISR — Frame-End Watchdog
//
// Fires when approx 209 ms of silence has elapsed since the last edge.  This
// signals that the current RC-5 burst has finished and the frame is ready
// for decoding.
//*****************************************************************************
static void SysTick_Handler(void)
{
    // Flag a completed frame to the main-loop consumer
    ir_timer_counter = 1;

    // Reset the counter so the watchdog restarts cleanly on the next edge
    HWREG(NVIC_ST_CURRENT) = 0;
}

//*****************************************************************************
//                      GPIO ISR — Edge Capture
//
// Fires on every rising and falling edge of the IR receiver output
// (GPIOA1 PIN_07).  Measures the time since the previous edge with SysTick
// and stores (duration | polarity) into ir_pulse_buffer.
//*****************************************************************************
static void Remote_Handler(void)
{
    // Clear the GPIO interrupt flag for this port
    unsigned long status = MAP_GPIOIntStatus(GPIOA1_BASE, true);
    MAP_GPIOIntClear(GPIOA1_BASE, status);

    // Elapsed ticks since the last edge (SysTick counts down from RELOAD)
    uint32_t time_ticks = IR_SYSTICK_RELOAD - HWREG(NVIC_ST_CURRENT);

    // Restart SysTick for the next measurement window
    HWREG(NVIC_ST_CURRENT) = 0;

    if (ir_timer_counter == 0) {
        // Mid-frame: record the pulse if there is space
        if (ir_pulse_idx < IR_PULSE_BUF_SIZE) {
            int      pin_val = MAP_GPIOPinRead(GPIOA1_BASE, GPIO_PIN_7);
            uint32_t time_us = time_ticks / IR_TICKS_PER_US;

            // Encode polarity in MSB (bit 31):
            //   pin LOW  now, the pulse that just ended was HIGH (bit31 = 1)
            //   pin HIGH now, the pulse that just ended was LOW (bit31 = 0)
            if (pin_val == 0) {
                ir_pulse_buffer[ir_pulse_idx] = time_us | 0x80000000u;
            } else {
                ir_pulse_buffer[ir_pulse_idx] = time_us;
            }
            ir_pulse_idx++;
        }
    } else {
        // SysTick watchdog fired between edges, new transmission arriving; start fresh!
        ir_timer_counter = 0;
        ir_pulse_idx     = 0;
    }
}

//*****************************************************************************
//                      Public API Implementation
//*****************************************************************************
void IR_Init(void)
{
    // ---- 1. SysTick: microsecond pulse-width timer -------------------------
    SysTickIntRegister(SysTick_Handler);

    HWREG(NVIC_ST_RELOAD)  = IR_SYSTICK_RELOAD - 1;
    HWREG(NVIC_ST_CURRENT) = 0;  // Reset counter

    // Enable processor clock source, interrupt, and counter
    HWREG(NVIC_ST_CTRL) |= (NVIC_ST_CTRL_CLK_SRC |
                             NVIC_ST_CTRL_INTEN   |
                             NVIC_ST_CTRL_ENABLE  );

    // ---- 2. GPIOA1 PIN_07: both-edges interrupt for the IR receiver --------
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1, PRCM_RUN_MODE_CLK);
    while (!MAP_PRCMPeripheralStatusGet(PRCM_GPIOA1));

    MAP_GPIOIntRegister(GPIOA1_BASE, Remote_Handler);
    MAP_GPIOIntTypeSet(GPIOA1_BASE, GPIO_PIN_7, GPIO_BOTH_EDGES);
    MAP_GPIOIntEnable(GPIOA1_BASE, GPIO_PIN_7);
}

bool IR_MessageReady(void)
{
    return (ir_timer_counter == 1 && ir_pulse_idx > 0);
}

int IR_Decode(void)
{
    if (ir_pulse_idx < 10) return -1;   // Too few edges for a valid RC-5 frame

    uint32_t code        = 0;
    int      bits        = 0;
    int      half_waiting = 0;
    int      i;

    for (i = 0; i < (int)ir_pulse_idx && bits < 14; i++) {
        uint32_t t    = ir_pulse_buffer[i] & 0x7FFFFFFFu;
        int      high = (ir_pulse_buffer[i] & 0x80000000u) ? 1 : 0;
        // high == 1: pulse that ended was HIGH (no carrier)
        // high == 0:  pulse that ended was LOW  (carrier active)

        // Discard noise and inter-frame gaps outside valid RC-5 range
        if (t < IR_T_NOISE_MIN || t > IR_T_LONG_MAX) continue;

        int is_short = (t < IR_T_SHORT_MAX);

        if (is_short) {
            // A short pulse covers one half-bit interval
            if (!half_waiting) {
                // First half of a new bit - wait for the second half
                half_waiting = 1;
            } else {
                // Second half: bit value equals the polarity of this half
                //   RC-5 "1": second half HIGH; high == 1
                //   RC-5 "0": second half LOW; high == 0
                code = (code << 1) | (unsigned int)high;
                bits++;
                half_waiting = 0;
            }
        } else {
            // A long pulse spans two half-bit intervals at the same polarity
            if (!half_waiting) {
                // Long pulse at a full-bit boundary. treat as first half only
                half_waiting = 1;
            } else {
                // Complete the pending bit; the same polarity acts as the
                // first half of the NEXT bit, so stay in half_waiting = 1
                code = (code << 1) | (unsigned int)high;
                bits++;
                half_waiting = 1;   // first half of next bit already seen
            }
        }
    }

    if (bits < 14) return -1;   // Incomplete frame
    return (int)(code & 0x3FFFu);
}

int IR_FetchCmd(int rc5_code)
{
    if (rc5_code < 0) return -1;
    // Return the full 8-bit value (includes address bits as used by this
    // remote — matches the IR_BTN_* constants in ir_receiver.h)
    return rc5_code & 0xFF;
}

void IR_Reset(void)
{
    ir_timer_counter = 0;
    ir_pulse_idx     = 0;
}
