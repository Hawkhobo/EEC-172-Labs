// Driver for the RDA5807M Single-Chip FM Radio Tuner
// Target: TI CC3200 (CCS v12.5), driverlib I2C API
//
// Datasheet references:
//   2.5  Control Interface  – I2C address, fixed start registers
//   5.1  I2C Interface Timing
//   6.0    Register Definition (REG 02H, 03H write; 0AH, 0BH read)
// ---------------------------------------------------------------------------

#include "rda5807m.h"

// Driverlib
#include "i2c.h"
#include "pin.h"
#include "hw_memmap.h"
#include "hw_types.h"
#include "hw_i2c.h"
#include "prcm.h"
#include "rom_map.h"
#include "utils.h"

// Common interface (UART terminal output)
#include "uart_if.h"


//*****************************************************************************
//                      Private Constants
//*****************************************************************************
#define RDA5807M_I2C_ADDR   0x10u            // RDA5807M 7-bit I2C address
#define SYS_CLK_HZ          80000000UL       // CC3200 system clock used to configure I2C baud rate

// Maximum poll iterations when waiting for the STC (Seek/Tune Complete) bit.
// Each iteration includes a ~1 ms software delay.
// 200 ms total is well beyond the chip's typical tune time.
#define STC_POLL_MAX        200

// REG 02H – sent as two bytes (high byte first, then low byte)
//
//  Bit 15  DHIZ        = 1  (audio output active, not high-Z)
//  Bit 14  DMUTE       = 1  (audio unmuted)
//  Bit 13  MONO        = 0  (attempt stereo)
//  Bit 12  BASS        = 0  (bass boost off)
//  Bit 11  RCLK_NC     = 0  (RCLK always supplied)
//  Bit 10  RCLK_DIRECT = 0  (normal oscillator mode)
//  Bit  9  SEEKUP      = 0
//  Bit  8  SEEK        = 0
//  Bit  7  SKMODE      = 0
//  Bits 6:4 CLK_MODE   = 000 (32.768 kHz reference – default xtal)
//  Bit  3  RDS_EN      = 0  (RDS off for now)
//  Bit  2  NEW_METHOD   = 1  (improved demodulation, ~+1 dB sensitivity)
//  Bit  1  SOFT_RESET  = 0
//  Bit  0  ENABLE      = 1
#define REG02_NORMAL_HI     0xC0u   // bits 15:8  = 1100 0000
#define REG02_NORMAL_LO     0x05u   // bits  7:0  = 0000 0101

// REG 02H with SOFT_RESET asserted (bit 1 = 1)
#define REG02_RESET_HI      0xC0u
#define REG02_RESET_LO      0x07u   // bits  7:0  = 0000 0111

// REG 03H – channel select + tune trigger
//
//  Bits 15:6  CHAN[9:0]  – computed at runtime from target frequency
//  Bit   5    DIRECT_MODE = 0  (normal)
//  Bit   4    TUNE        = 1  (start tune operation)
//  Bits  3:2  BAND[1:0]  = 00  (US/Europe 87–108 MHz)
//  Bits  1:0  SPACE[1:0] = 00  (100 kHz spacing)
//
// Low-byte mask for the constant control bits (TUNE=1, BAND=00, SPACE=00)
#define REG03_CTRL_LO       0x10u   // 0001 0000

// REG 0AH read-back bit positions (within the 16-bit register, high byte first)
// High byte (bits 15:8 of REG 0x0A):
//   bit 6 of high byte  =  bit 14 of register  =  STC
//   bit 2 of high byte  =  bit 10 of register  =  ST (stereo indicator)
#define REG0A_HI_STC_BIT    0x40u   // STC in the high byte
#define REG0A_HI_ST_BIT     0x04u   // Stereo indicator in high byte

// REG 0BH read-back bit positions (high byte):
//   bits 7:1 of high byte  =  bits 15:9 of register  =  RSSI[6:0]
//   bit  0 of high byte    =  bit  8 of register      =  FM_TRUE
#define REG0B_HI_FMTRUE_BIT 0x01u   // FM_TRUE in the high byte
#define REG0B_HI_RSSI_SHIFT 1       // RSSI[6:0] starts at bit 1 of high byte


//*****************************************************************************
//                      Private API Helpers
//*****************************************************************************

/**
 * i2c_write_bytes
 *
 * Writes `len` bytes from `buf[]` to the RDA5807M over I2C.
 * The chip auto-starts at REG 0x02 and increments internally.
 * Uses CC3200 burst-send API (driverlib i2c.h).
 *
 * @return  RDA5807M_OK or RDA5807M_ERR_I2C_WRITE
 */
static int i2c_write_bytes(const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (len == 0) return RDA5807M_OK;

    MAP_I2CMasterSlaveAddrSet(I2CA0_BASE, RDA5807M_I2C_ADDR, false /* write */);

    if (len == 1) {
        MAP_I2CMasterDataPut(I2CA0_BASE, buf[0]);
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return RDA5807M_ERR_I2C_WRITE;
        return RDA5807M_OK;
    }

    // Multi-byte: START + first byte
    MAP_I2CMasterDataPut(I2CA0_BASE, buf[0]);
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return RDA5807M_ERR_I2C_WRITE;

    // Middle bytes (if any)
    for (i = 1; i < len - 1; i++) {
        MAP_I2CMasterDataPut(I2CA0_BASE, buf[i]);
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_SEND_CONT);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return RDA5807M_ERR_I2C_WRITE;
    }

    // Last byte + STOP
    MAP_I2CMasterDataPut(I2CA0_BASE, buf[len - 1]);
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return RDA5807M_ERR_I2C_WRITE;

    return RDA5807M_OK;
}

/**
 * i2c_read_bytes
 *
 * Reads `len` bytes from the RDA5807M into `buf[]`.
 * The chip auto-starts at REG 0x0A and increments internally.
 * Uses CC3200 burst-receive API (driverlib i2c.h).
 *
 * @return  RDA5807M_OK or RDA5807M_ERR_I2C_READ
 */
static int i2c_read_bytes(uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (len == 0) return RDA5807M_OK;

    MAP_I2CMasterSlaveAddrSet(I2CA0_BASE, RDA5807M_I2C_ADDR, true /* read */);

    if (len == 1) {
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return RDA5807M_ERR_I2C_READ;
        buf[0] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);
        return RDA5807M_OK;
    }

    // Multi-byte: START + receive first byte
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_START);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return RDA5807M_ERR_I2C_READ;
    buf[0] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);

    // Middle bytes (if any)
    for (i = 1; i < len - 1; i++) {
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_CONT);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return RDA5807M_ERR_I2C_READ;
        buf[i] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);
    }

    // Last byte: send NACK + STOP
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_FINISH);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return RDA5807M_ERR_I2C_READ;
    buf[len - 1] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);

    return RDA5807M_OK;
}

//*****************************************************************************
//                      Public API
//*****************************************************************************
int RDA5807M_Init(void)
{
    int ret;

    // ---- 1. Configure I2C pins on the CC3200 --------------------------------
    // PIN_01 = I2C_SCL  (Mode 1),  PIN_02 = I2C_SDA  (Mode 1)
    // Open-drain is handled internally; external 10k pull-ups to 3.3V required.
    MAP_PinTypeI2C(PIN_01, PIN_MODE_1);
    MAP_PinTypeI2C(PIN_02, PIN_MODE_1);

    // ---- 2. Enable and initialise the I2C master peripheral -----------------
    MAP_PRCMPeripheralClkEnable(PRCM_I2CA0, PRCM_RUN_MODE_CLK);
    while (!MAP_PRCMPeripheralStatusGet(PRCM_I2CA0));

    // false = 100 kHz standard mode (RDA5807M supports up to 400 kHz)
    MAP_I2CMasterInitExpClk(I2CA0_BASE, SYS_CLK_HZ, false);

    // ---- 3. Issue a soft reset to the RDA5807M ------------------------------
    // Datasheet §2.4: assert SOFT_RESET (bit 1 of REG 02H) then deassert.
    // Write order: REG02H_HI, REG02H_LO  (chip writes starting at 0x02).
    uint8_t reset_seq[2] = { REG02_RESET_HI, REG02_RESET_LO };
    ret = i2c_write_bytes(reset_seq, 2);
    if (ret != RDA5807M_OK) {
        UART_PRINT("[RDA5807M] Init: soft-reset write failed (%d)\n\r", ret);
        return ret;
    }

    // Allow the chip ~5 ms to complete its internal reset
    MAP_UtilsDelay(SYS_CLK_HZ / 3 / 200);   // ~5 ms at 80 MHz

    // ---- 4. Re-enable in normal operating mode (SOFT_RESET deasserted) ------
    uint8_t enable_seq[2] = { REG02_NORMAL_HI, REG02_NORMAL_LO };
    ret = i2c_write_bytes(enable_seq, 2);
    if (ret != RDA5807M_OK) {
        UART_PRINT("[RDA5807M] Init: enable write failed (%d)\n\r", ret);
        return ret;
    }

    // Brief settling delay after power-up enable
    MAP_UtilsDelay(SYS_CLK_HZ / 3 / 100);   // ~10 ms

    UART_PRINT("[RDA5807M] Init complete.\n\r");
    return RDA5807M_OK;
}



int RDA5807M_TuneFrequency(float freq_mhz)
{
    int     ret;
    uint8_t rx[4];          // Holds 0x0A high/low + 0x0B high/low
    int     poll;

    // ---- 1. Validate frequency ---------------------------------------------
    if (freq_mhz < RDA5807M_BAND_MIN_MHZ || freq_mhz > RDA5807M_BAND_MAX_MHZ) {
        UART_PRINT("[RDA5807M] Frequency %.1f MHz out of range (87.0–108.0 MHz)\n\r",
                   (double)freq_mhz);
        return RDA5807M_ERR_FREQ;
    }

    // ---- 2. Calculate CHAN[9:0] ---------------------------------------------
    // Datasheet SEC 6, REG 03H:
    //   Frequency = ChannelSpacing_kHz * CHAN + 87.0 MHz   (BAND=00)
    //   ChannelSpacing = 100 kHz  (SPACE=00)
    // => CHAN = (freq_mhz - 87.0) / 0.1
    uint16_t chan = (uint16_t)(((freq_mhz - RDA5807M_BAND_MIN_MHZ) / 0.1f) + 0.5f);

    // CHAN sits in bits [15:6] of REG 03H
    uint16_t reg03 = (uint16_t)((chan << 6) | REG03_CTRL_LO);
    //                            ^^^^^^^^^     ^^^^^^^^^^^^
    //                            CHAN[9:0]     TUNE=1, BAND=00, SPACE=00

    uint8_t reg03_hi = (uint8_t)(reg03 >> 8);
    uint8_t reg03_lo = (uint8_t)(reg03 & 0xFF);

    // ---- 3. Write REG 02H + REG 03H in one I2C transaction ------------------
    // I2C write always begins at REG 0x02 (datasheet SEC 2.5).
    // Byte order: [02H_hi, 02H_lo, 03H_hi, 03H_lo]
    uint8_t tx[4] = {
        REG02_NORMAL_HI,
        REG02_NORMAL_LO,
        reg03_hi,
        reg03_lo
    };

    UART_PRINT("[RDA5807M] Tuning to %.1f MHz (CHAN=%u, REG03=0x%04X)...\n\r",
               (double)freq_mhz, chan, (unsigned)reg03);

    ret = i2c_write_bytes(tx, 4);
    if (ret != RDA5807M_OK) {
        UART_PRINT("[RDA5807M] Tune write failed (%d)\n\r", ret);
        return ret;
    }

    // ---- 4. Poll STC (Seek/Tune Complete) in REG 0x0A ----------------------
    // Datasheet SEC 6, REG 0AH bit 14 (= bit 6 of the high byte):
    //   STC = 1 when tune is complete.
    // Also read REG 0x0B (RSSI, FM_TRUE) at the same time (4 bytes total).
    for (poll = 0; poll < STC_POLL_MAX; poll++) {
        MAP_UtilsDelay(SYS_CLK_HZ / 3 / 1000);  // ~1 ms per poll iteration

        ret = i2c_read_bytes(rx, 4);
        if (ret != RDA5807M_OK) {
            UART_PRINT("[RDA5807M] Read failed during STC poll (%d)\n\r", ret);
            return ret;
        }

        // rx[0] = REG 0x0A high byte, rx[1] = REG 0x0A low byte
        // rx[2] = REG 0x0B high byte, rx[3] = REG 0x0B low byte
        if (rx[0] & REG0A_HI_STC_BIT) {
            break;  // Tune complete
        }
    }

    if (poll >= STC_POLL_MAX) {
        UART_PRINT("[RDA5807M] Timeout waiting for STC after %d ms\n\r",
                   STC_POLL_MAX);
        return RDA5807M_ERR_TIMEOUT;
    }

    // ---- 5. Parse and report status ----------------------------------------
    //
    // REG 0x0A high byte layout:
    //   bit 7 = RDSR, bit 6 = STC, bit 5 = SF, bit 4 = RDSS,
    //   bit 3 = BLK_E, bit 2 = ST, bits 1:0 = READCHAN[9:8]
    //
    // REG 0x0B high byte layout:
    //   bits 7:1 = RSSI[6:0], bit 0 = FM_TRUE
    //
    uint8_t  stc      = (rx[0] & REG0A_HI_STC_BIT)    ? 1 : 0;
    uint8_t  stereo   = (rx[0] & REG0A_HI_ST_BIT)      ? 1 : 0;
    uint8_t  fm_true  = (rx[2] & REG0B_HI_FMTRUE_BIT)  ? 1 : 0;
    uint8_t  rssi     = (uint8_t)(rx[2] >> REG0B_HI_RSSI_SHIFT);  // 0–127

    // Reconstruct the actual tuned channel from READCHAN[9:0]
    uint16_t readchan = (uint16_t)(((rx[0] & 0x03u) << 8) | rx[1]);
    float    actual_mhz = RDA5807M_BAND_MIN_MHZ + (readchan * 0.1f);

    UART_PRINT("--------------------------------------------------\n\r");
    UART_PRINT("[RDA5807M] Tune complete after %d ms\n\r", poll);
    UART_PRINT("  Requested : %.1f MHz\n\r",  (double)freq_mhz);
    UART_PRINT("  Confirmed : %.1f MHz\n\r",  (double)actual_mhz);
    UART_PRINT("  STC       : %s\n\r",  stc     ? "YES (tune complete)" : "NO");
    UART_PRINT("  RSSI      : %u (0=min, 127=max, log scale)\n\r", (unsigned)rssi);
    UART_PRINT("  Stereo    : %s\n\r",  stereo  ? "YES" : "NO");
    UART_PRINT("  FM_TRUE   : %s\n\r",  fm_true ? "YES (valid station)" : "NO (weak/no signal)");
    UART_PRINT("--------------------------------------------------\n\r");

    // ---- 6. Return result --------------------------------------------------
    if (!fm_true) {
        // The chip tuned successfully but did not detect a valid FM station.
        // This is expected without an antenna or in a weak-signal environment.
        UART_PRINT("[RDA5807M] WARNING: station not confirmed (FM_TRUE=0). "
                   "Check antenna connection.\n\r");
        return RDA5807M_ERR_NO_SIGNAL;
    }

    UART_PRINT("[RDA5807M] Playback active on %.1f MHz.\n\r", (double)freq_mhz);
    return RDA5807M_OK;
}

