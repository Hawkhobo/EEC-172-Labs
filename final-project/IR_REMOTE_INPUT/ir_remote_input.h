// FM Frequency String Accumulator for IR Remote Numeric Keypad
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// OVERVIEW
// --------
// This module accepts numeric IR button presses one at a time and builds up
// an FM frequency string that can be submitted to TEA5767_TuneFrequency().
// Note it does not support multi-tap
//
// BUTTON MAPPING
// -------------------------------------------
//   IR_BTN_0        -> '0'
//   IR_BTN_1        -> '.'   (decimal point, per project spec)
//   IR_BTN_2 - 9   -> '2'-'9'
//   IR_BTN_SELECT   -> submit (tune to entered frequency)
//   IR_BTN_BACK     -> delete last character
//
//
// INPUT CONSTRAINTS
// -----------------
//   - Maximum 7 characters ("108.00\0" is the longest valid US FM frequency)
//   - At most one decimal point (subsequent button-1 presses are ignored)
//   - Cannot begin with a decimal point
//   - Range validation (87.5 – 108.0 MHz) is delegated to TEA5767_TuneFrequency()
//
// OLED DISPLAY
// ------------
//   Call IR_FreqInput_GetStr() to obtain the live buffer and show it in the
//   station field while input is in progress:
//     oled_ui_update_radio(IR_FreqInput_GetStr(), NULL, NULL, NULL, 0, 0);
//
// CALL SEQUENCE
// -------------
//   IR_FreqInput_PressDigit(cmd)  -- called on each numeric button press
//   IR_FreqInput_Delete()         -- called on IR_BTN_BACK
//   IR_FreqInput_Submit()         -- called on IR_BTN_SELECT; returns float
//   IR_FreqInput_Reset()          -- called to cancel/clear at any time
//   IR_FreqInput_IsActive()       -- true when buffer is non-empty

#ifndef IR_FREQ_INPUT_H_
#define IR_FREQ_INPUT_H_

#include <stdbool.h>

//*****************************************************************************
//                      Buffer Sizing
//*****************************************************************************
// US FM band max: "108.00" = 6 chars + NUL = 7; round to 8 for safety.
#define IR_FREQ_MAX_LEN     8

//*****************************************************************************
//                      Public API
//*****************************************************************************

/**
 * IR_FreqInput_Reset
 *
 * Clears the accumulator buffer. Call this to cancel an in-progress entry
 * or after a successful/failed submission.
 */
void IR_FreqInput_Reset(void);

/**
 * IR_FreqInput_PressDigit
 *
 * Appends the character corresponding to the given IR command code:
 *   IR_BTN_0        -> '0'
 *   IR_BTN_1        -> '.'
 *   IR_BTN_2 – 9   -> '2' – '9'
 *
 * Rejects the character silently (returns false) if:
 *   - ir_cmd is not a recognised digit button
 *   - the buffer is already full (7 characters)
 *   - a second decimal point is requested
 *   - a decimal point would be the very first character
 *
 * @param  ir_cmd   Return value of IR_FetchCmd() for a numeric button.
 * @return true  if the character was accepted and appended.
 *         false if the character was rejected.
 */
bool IR_FreqInput_PressDigit(int ir_cmd);

/**
 * IR_FreqInput_Delete
 *
 * Removes the last character from the buffer (backspace).
 * Has no effect if the buffer is already empty.
 */
void IR_FreqInput_Delete(void);

/**
 * IR_FreqInput_Submit
 *
 * Parses the accumulated string as a floating-point FM frequency.
 * Does NOT call TEA5767_TuneFrequency() — that is main()'s responsibility,
 * so error handling and OLED updates stay in one place.
 *
 * The buffer is cleared on a successful parse (whether or not the
 * frequency is in-band is left for TEA5767 to decide).
 *
 * @return  Parsed frequency in MHz (e.g. 90.3f) on success.
 *          -1.0f if the buffer is empty or cannot be parsed as a number.
 */
float IR_FreqInput_Submit(void);

/**
 * IR_FreqInput_GetStr
 *
 * Returns a NUL-terminated pointer to the current accumulator contents.
 * The pointer is valid until the next call to any IR_FreqInput_* function.
 * Use this to live-preview the entry on the OLED station field.
 *
 * @return  Read-only C string, e.g. "90." while the user is still typing.
 */
const char *IR_FreqInput_GetStr(void);

/**
 * IR_FreqInput_IsActive
 *
 * @return  true  if at least one character has been entered (buffer non-empty).
 *          false if the buffer is empty (no pending input).
 */
bool IR_FreqInput_IsActive(void);

#endif
