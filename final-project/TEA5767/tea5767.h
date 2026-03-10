// tea5767.h
// Driver for the TEA5767HN Single-Chip FM Radio Tuner
// Communicates over I2C (I2CA0) on the CC3200 LaunchPad
//
// Hardware wiring:
//   CC3200 PIN_01  <-->  TEA5767 CLOCK (pin 9)   [I2C SCL, Mode 1]
//   CC3200 PIN_02  <-->  TEA5767 DATA  (pin 8)   [I2C SDA, Mode 1]
//   CC3200 3.3V    <-->  TEA5767 VCCA (pin 34), VCCD (pin 7), VCC(VCO) (pin 5)
//   CC3200 GND     <-->  TEA5767 AGND (pin 33), DGND (pin 6), RFGND (pin 36)
//   TEA5767 BUSMODE (pin 12) tied LOW  -> selects I2C mode
//   TEA5767 BUSENABLE (pin 13) tied HIGH -> bus active
//   4.7k pull-up resistors on SDA and SCL to 3.3V (per datasheet II-B)
//   Module's built-in FM antenna connects to RFI1/RFI2 (pins 35/37)
//
// I2C address: 0x60 (7-bit), fixed by the chip (datasheet 8.2, Table 4:
//   address bits = 1100000b, R/W = bit 0).
//
// Protocol: No internal register pointer. Every write sends exactly 5 bytes;
// every read receives exactly 5 bytes (datasheet 8.2, 8.5).
//
// Reference frequency assumed: 32.768 kHz crystal (XTAL bit = 1, PLLREF = 0).
// PLL formula (high-side injection, datasheet §8.4):
//   N = 4 * (fRF_Hz + fIF_Hz) / fref_Hz
//   fIF = 225,000 Hz;  fref = 32,768 Hz
//
// ---------------------------------------------------------------------------
#ifndef TEA5767_H_
#define TEA5767_H_

#include <stdint.h>
#include <stdbool.h>
#include <common.h>

// ---- Return codes ----------------------------------------------------------
#define TEA5767_OK              0   // Tune complete, station detected
#define TEA5767_ERR_FREQ       -1   // Frequency out of US/Europe band (87.5–108 MHz)
#define TEA5767_ERR_I2C_WRITE  -2   // I2C write transaction failed
#define TEA5767_ERR_I2C_READ   -3   // I2C read transaction failed
#define TEA5767_ERR_TIMEOUT    -4   // Ready Flag (RF) never set within timeout
#define TEA5767_ERR_NO_SIGNAL  -5   // RF set but signal level too low (weak/no station)

// ---- Band limits (US/Europe, datasheet 8.4 BL=0: 87.5–108 MHz) -----------
#define TEA5767_BAND_MIN_MHZ   87.5f
#define TEA5767_BAND_MAX_MHZ  108.0f

// ---- Public API ------------------------------------------------------------

/**
 * TEA5767_Init
 *
 * Initialises the CC3200 I2CA0 peripheral for communication with the TEA5767:
 *   1. Configures I2C pins (PIN_01 = SCL, PIN_02 = SDA, 100 kHz).
 *   2. Sends a 5-byte wake-up / default configuration frame to bring the
 *      module out of standby with mute off and stereo enabled.
 *
 * Must be called once before TEA5767_TuneFrequency().
 *
 * @return  TEA5767_OK on success, TEA5767_ERR_I2C_WRITE on failure.
 */
int TEA5767_Init(void);

/**
 * TEA5767_TuneFrequency
 *
 * Tunes the TEA5767 to the supplied FM frequency and waits for the chip to
 * assert the Ready Flag (RF bit in read byte 1), confirming the PLL has locked.
 * Prints a full status summary to the CCS UART terminal using UART_PRINT().
 *
 * Band assumed: US/Europe (87.5–108 MHz). High-side LO injection (HLSI=1).
 * Reference clock: 32.768 kHz crystal (XTAL=1, PLLREF=0).
 *
 * @param  freq_mhz  Desired station in MHz, e.g. 90.3f
 *
 * @return  TEA5767_OK             – PLL locked, station detected
 *          TEA5767_ERR_FREQ       – frequency outside 87.5–108.0 MHz
 *          TEA5767_ERR_I2C_WRITE  – I2C write failed
 *          TEA5767_ERR_I2C_READ   – I2C read failed during RF polling
 *          TEA5767_ERR_TIMEOUT    – chip did not assert RF within timeout
 *          TEA5767_ERR_NO_SIGNAL  – RF set but signal level (LEV) is very low
 */
int TEA5767_TuneFrequency(float freq_mhz);

/**
 * TEA5767_SetMute
 *
 * Mutes or unmutes both audio channels by setting or clearing the MUTE bit
 * (write byte 1, bit 7) while preserving all other register settings.
 *
 * The TEA5767 has no read-modify-write capability — every write overwrites
 * all five bytes. This driver keeps a shadow copy of the last written frame
 * (g_tx_shadow[]) so that only the MUTE bit is changed here.
 *
 * TEA5767_Init() must have been called before this function.
 *
 * @param  mute   true  = mute both channels (audio silenced)
 *                false = unmute (audio restored)
 *
 * @return  TEA5767_OK            – write succeeded
 *          TEA5767_ERR_I2C_WRITE – I2C write failed (mute state unchanged)
 */
int TEA5767_SetMute(bool mute);

/**
 * TEA5767_GetSignalStrength
 *
 * Issues a 5-byte I2C read and extracts the 4-bit signal-level ADC (LEV,
 * read byte 4 bits [7:4]).  The raw value (0-15) is scaled linearly to the
 * 0-100 range expected by oled_ui_update_radio().
 *
 *   scaled = (raw_lev * 100 + 7) / 15    (rounded integer arithmetic)
 *
 * Typical values:
 *   0  - 20  : no / very weak signal (check antenna)
 *   30 - 60  : moderate reception
 *   70 - 100 : strong local station
 *
 * Call this after TEA5767_TuneFrequency() has already locked the PLL.
 * It does NOT poll the Ready Flag — it is for periodic status reads only.
 *
 * @return  Signal strength 0-100 on success.
 *          TEA5767_ERR_I2C_READ (negative) on I2C failure.
 */
int TEA5767_GetSignalStrength(void);

#endif
