// Driver for the RDA5807M Single-Chip FM Radio Tuner
// Communicates over I2C (I2CA0) on the CC3200 LaunchPad

// Hardware wiring (add to PinMuxConfig / SysConfig):
//   CC3200 PIN_01  <-->  RDA5807M SCLK [I2C SCL, Mode 1]
//   CC3200 PIN_02  <-->  RDA5807M SDIO [I2C SDA, Mode 1]
//   CC3200 3.3V    <-->  RDA5807M VDD 
//   CC3200 GND     <-->  RDA5807M GND  
//   Antenna wire   <-->  RDA5807M FMIN [ANT]
//   32.768 kHz xtal OR reference clock <--> RDA5807M RCLK (pin 6)
//   10k pull-up resistors on SDA and SCL to 3.3V

// I2C address: 0x10 (7-bit)
// Write starts at REG 0x02; Read starts at REG 0x0A

#ifndef RDA5807M_H_
#define RDA5807M_H_

#include <stdint.h>
#include <stdbool.h>

// Example SDK code for UART_PRINT
#include <common.h>

//*****************************************************************************
//                      RDA5807M API Return Codes
//*****************************************************************************
#define RDA5807M_OK             0   // Tune complete, station detected
#define RDA5807M_ERR_FREQ      -1   // Frequency out of the 87–108 MHz US band
#define RDA5807M_ERR_I2C_WRITE -2   // I2C write transaction failed
#define RDA5807M_ERR_I2C_READ  -3   // I2C read transaction failed
#define RDA5807M_ERR_TIMEOUT   -4   // STC (Seek/Tune Complete) bit never set
#define RDA5807M_ERR_NO_SIGNAL -5   // Tune completed but FM_TRUE not asserted

// ---- Band limits (US/Europe BAND=00: 87–108 MHz, 100 kHz spacing) ----------
#define RDA5807M_BAND_MIN_MHZ   87.0f
#define RDA5807M_BAND_MAX_MHZ  108.0f

//*****************************************************************************
//                      RDA5807M Public API
//*****************************************************************************

/**
 * RDA5807M_Init
 *
 * Initialises the CC3200 I2CA0 peripheral and the RDA5807M:
 *   1. Configures I2C pins (PIN_01 = SCL, PIN_02 = SDA, 100 kHz).
 *   2. Issues a soft-reset to the radio then re-enables it in a known state:
 *        DHIZ=1 (audio output active), DMUTE=1 (unmuted), ENABLE=1.
 *
 * Must be called once before RDA5807M_TuneFrequency().
 *
 * @return  RDA5807M_OK on success, RDA5807M_ERR_I2C_WRITE on failure.
 */
int RDA5807M_Init(void);

/**
 * RDA5807M_TuneFrequency
 *
 * Tunes the RDA5807M to the supplied FM frequency and waits for the chip to
 * confirm the tune is complete (STC bit in REG 0x0A).
 * Prints a status summary to the CCS UART terminal using UART_PRINT().
 *
 * Band / spacing assumed: US/Europe 87–108 MHz, 100 kHz channel spacing.
 *
 * @param  freq_mhz  Desired station in MHz, e.g. 90.3f
 *
 * @return  RDA5807M_OK            – tune complete, FM signal detected
 *          RDA5807M_ERR_FREQ      – frequency outside 87–108 MHz
 *          RDA5807M_ERR_I2C_WRITE – I2C write failed
 *          RDA5807M_ERR_I2C_READ  – I2C read failed during STC polling
 *          RDA5807M_ERR_TIMEOUT   – chip did not assert STC within timeout
 *          RDA5807M_ERR_NO_SIGNAL – STC set but FM_TRUE not asserted (weak/no signal)
 */
int RDA5807M_TuneFrequency(float freq_mhz);

#endif /* RDA5807M_H_ */
