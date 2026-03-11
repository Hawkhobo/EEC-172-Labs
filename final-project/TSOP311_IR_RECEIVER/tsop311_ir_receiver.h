// IR Receiver Driver — RC-5 Protocol Decoder
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// OVERVIEW
// --------
// This module decodes NEC/RC-5 IR remote signals received on GPIOA1 PIN_07
// using a SysTick-based microsecond pulse timer and a GPIO edge ISR.
//
// HARDWARE
// --------
//   IR receiver OUT  --->  CC3200 PIN_07  (GPIOA1, bit 7)
//   3.3V / GND as required by the receiver module.
//
// PROTOCOL NOTES (RC-5)
// ---------------------
//   1) Each frame = 14 biphase-Mark bits: S1 S2 T A4 A3 A2 A1 A0 C5 C4 C3 C2 C1 C0
//   2) Unit interval T_UNIT = 889 microseconds; long pulse = 2 × T_UNIT = 1778 microseconds
//   3) Command field = bits [5:0]; address field = bits [10:6]
//   4) The toggle bit (bit 11) flips on every new key press and stays the same
//       when a key is held — use IR_FetchCmd() to strip it out.

#ifndef IR_RECEIVER_H_
#define IR_RECEIVER_H_

#include <stdint.h>
#include <stdbool.h>

//*****************************************************************************
//                      RC-5 Protocol Timing Constants
//*****************************************************************************
#define IR_T_UNIT        889    // RC-5 unit interval
#define IR_T_SHORT_MAX  1334    // Upper bound for a one-T pulse
#define IR_T_NOISE_MIN   300    // Minimum pulse duration before noise filter
#define IR_T_LONG_MAX   2500    // Upper bound for a two-T pulse

//*****************************************************************************
//                      RC-5 Button Command Codes
//
// bits [5:0] of a decoded RC-5 frame for the TV remote.
// Pass any IR_FetchCmd() result  through these constants to identify the key that was pressed.
//*****************************************************************************

// --- Navigation ---
//#define IR_BTN_VOL_UP       220    // Vol + : Volume Up
//#define IR_BTN_VOL_DOWN     221    // Vol - : vOLUME Down
#define IR_BTN_LEFT         221    // Left arrow : navigate to previous OLED view
#define IR_BTN_RIGHT        220    // Right arrow : navigate to next OLED view
#define IR_BTN_SELECT       160    // OK, return to Radio view (remote-input mode)

// --- Playback / Seek ---
//#define IR_BTN_CH_PLUS      221    // Next : seek upward to next station
//#define IR_BTN_CH_MINUS    220    // Prev : seek downward to previous station

// --- Auxiliary ---
#define IR_BTN_MUTE         229    // Mute toggle
#define IR_BTN_LAST         184    // Back / Return (also Delete in remote-input mode)

// --- Content Scrolling (BROWSE up/down buttons) ---
// distinct from VOL +/-
// Used to scroll up/down inside text-heavy OLED views (bio, lyrics, lists).
#define IR_BTN_UP     188
#define IR_BTN_DOWN   189

// --- FM Tuning ---
// *** CALIBRATION NOTE ***
// If any code here does not match your remote, print IR_FetchCmd() over UART
// for each button press and update the constants below accordingly.
#define IR_BTN_0            252    // Digit 0
#define IR_BTN_1            253    // Digit 1
#define IR_BTN_2            248    // Digit 2
#define IR_BTN_3            249    // Digit 3
#define IR_BTN_4            244    // Digit 4
#define IR_BTN_5            245    // Digit 5
#define IR_BTN_6            240    // Digit 6
#define IR_BTN_7            241    // Digit 7
#define IR_BTN_8            236    // Digit 8
#define IR_BTN_9            237    // Digit 9
#define IR_BTN_DOT          129    // Decimal point '.', ENTER on remote

//*****************************************************************************
//                      Pulse Capture Buffer
//
// Shared between the GPIO ISR (Remote_Handler) and the decoder (IR_Decode).
// The main loop must call IR_Reset() after each decode to clear the buffer.
//*****************************************************************************
#define IR_PULSE_BUF_SIZE   128    // Max edges per RC-5 frame (14 bits, approx 28)

// Edge timings captured by the ISR.  Upper bit encodes polarity:
//   bit 31 = 1:  pulse that just ended was HIGH (no carrier)
//   bit 31 = 0:  pulse that just ended was LOW  (carrier active)
//   bits 30:0:  duration in microseconds
extern volatile uint32_t ir_pulse_buffer[IR_PULSE_BUF_SIZE];
extern volatile uint32_t ir_pulse_idx;

// Set to 1 by SysTick_Handler when approx 209 ms of silence is detected (frame end).
// Cleared by IR_Reset() / IR_MessageReady() consumer.
extern volatile int ir_timer_counter;

//*****************************************************************************
//                      Public API
//*****************************************************************************

/**
 * IR_Init
 *
 * Initialises all hardware required for IR reception:
 *   1. SysTick down-counter (80 MHz / 24-bit) for pulse-width measurement.
 *   2. GPIOA1 PIN_07 as a both-edges interrupt source feeding Remote_Handler.
 *
 * Must be called once after BoardInit() and PinMuxConfig().
 */
void IR_Init(void);

/**
 * IR_MessageReady
 *
 * Returns true when a complete IR burst has been captured (the SysTick
 * watchdog fired after the final edge) and there is at least one edge in
 * the buffer.  Intended for polling in the main loop.
 *
 * @return  true  — a frame is waiting to be decoded
 *          false — still receiving or buffer empty
 */
bool IR_MessageReady(void);

/**
 * IR_Decode
 *
 * Decodes the current contents of ir_pulse_buffer using the RC-5
 * biphase-Mark algorithm.
 *
 * @return  14-bit RC-5 frame word on success (bits [13:0] valid),
 *          or -1 if the frame is too short / incomplete.
 */
int IR_Decode(void);

/**
 * IR_FetchCmd
 *
 * Extracts the 6-bit command nibble from a decoded RC-5 frame.
 * The result maps directly to the IR_BTN_* constants defined above.
 *
 * @param  rc5_code  Return value of IR_Decode().
 *
 * @return  Command byte (0–63 in a standard RC-5 frame, or higher values
 *          depending on address bits retained by this implementation),
 *          or -1 if rc5_code is negative.
 */
int IR_FetchCmd(int rc5_code);

/**
 * IR_Reset
 *
 * Clears the pulse buffer and the frame-ready flag so the ISR can begin
 * capturing the next transmission.  Must be called after every IR_Decode().
 */
void IR_Reset(void);

#endif
