// FM Frequency String Accumulator — Implementation
// Jacob Feenstra & Chun-Ho Chen
// EEC172 WQ2026 Final Project
//
// See ir_freq_input.h for full documentation and usage.

#include <IR_REMOTE_INPUT/ir_remote_input.h>

#include "../TSOP311_IR_RECEIVER/tsop311_ir_receiver.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

// Single static instance — only one frequency can be composed at a time.
static char s_buf[IR_FREQ_MAX_LEN];   // NUL-terminated accumulator
static int  s_len;                     // Current character count (excludes NUL)
static bool s_has_decimal;             // True once a '.' has been appended

//*****************************************************************************
//                      Helpers
//*****************************************************************************
// Map an IR command code to the character it represents for frequency entry.
// Returns '\0' if ir_cmd is not a numeric/period button.
static char cmd_to_char(int ir_cmd)
{
    switch (ir_cmd) {
        case IR_BTN_0:  return '0';
        case IR_BTN_1:  return '.';
        case IR_BTN_2:  return '2';
        case IR_BTN_3:  return '3';
        case IR_BTN_4:  return '4';
        case IR_BTN_5:  return '5';
        case IR_BTN_6:  return '6';
        case IR_BTN_7:  return '7';
        case IR_BTN_8:  return '8';
        case IR_BTN_9:  return '9';
        default:        return '\0';
    }
}

//*****************************************************************************
//                      Public API
//*****************************************************************************
void IR_FreqInput_Reset(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_len         = 0;
    s_has_decimal = false;
}

bool IR_FreqInput_PressDigit(int ir_cmd)
{
    char ch = cmd_to_char(ir_cmd);

    // Unrecognized button
    if (ch == '\0') return false;

    // Buffer full — "108.00" is 6 chars, but be safe with the 7-char ceiling
    if (s_len >= IR_FREQ_MAX_LEN - 1) return false;

    // Reject a second decimal point
    if (ch == '.' && s_has_decimal) return false;

    // Reject a leading decimal point
    if (ch == '.' && s_len == 0) return false;

    // Accept the character
    s_buf[s_len] = ch;
    s_len++;
    s_buf[s_len] = '\0';

    if (ch == '.') s_has_decimal = true;

    return true;
}

void IR_FreqInput_Delete(void)
{
    if (s_len == 0) return;

    s_len--;
    if (s_buf[s_len] == '.') s_has_decimal = false;
    s_buf[s_len] = '\0';
}

float IR_FreqInput_Submit(void)
{
    if (s_len == 0) return -1.0f;

    // strtof sets `end` to the first non-numeric character.
    // A valid parse means end points to the NUL terminator.
    char  *end;
    float  freq = strtof(s_buf, &end);

    if (end == s_buf || *end != '\0') {
        // Could not parse a number (e.g. buffer was just ".")
        return -1.0f;
    }

    // Clear the buffer — next input starts fresh.
    IR_FreqInput_Reset();
    return freq;
}

const char *IR_FreqInput_GetStr(void)
{
    return s_buf;
}

bool IR_FreqInput_IsActive(void)
{
    return s_len > 0;
}
