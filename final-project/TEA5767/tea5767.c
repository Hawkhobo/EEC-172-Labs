// tea5767.c
// Driver for the TEA5767HN Single-Chip FM Radio Tuner
// Target: TI CC3200 (CCS v12.5), driverlib I2C API
//
// Datasheet references (TEA5767HN Rev 05, NXP, 26 January 2007):
//   8.2   I2C-bus protocol  – address byte, write/read framing
//   8.4   Write mode data   – 5-byte write register map (Tables 5–16)
//   8.5   Read mode data    – 5-byte read register map  (Tables 17–26)
//   15    I2C-bus characteristics
// ---------------------------------------------------------------------------

#include "tea5767.h"

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

// ---------------------------------------------------------------------------
// Private constants
// ---------------------------------------------------------------------------

// TEA5767HN 7-bit I2C address (datasheet 8.2, Table 4: "1100000b")
#define TEA5767_I2C_ADDR    0x60u

// CC3200 system clock
#define SYS_CLK_HZ          80000000UL

// Maximum poll iterations waiting for the Ready Flag (RF).
// Each iteration has a ~1 ms software delay; 300 ms total is well beyond
// the typical PLL lock time of the TEA5767.
#define RF_POLL_MAX         300

// ---------------------------------------------------------------------------
// PLL calculation constants (datasheet 8.4)
//
//   N = 4 * (fRF + fIF) / fref          [high-side injection, HLSI=1]
//
//   fIF  = 225,000 Hz  (fixed intermediate frequency)
//   fref = 32,768  Hz  (32.768 kHz crystal, XTAL=1, PLLREF=0)
//
// PLL word N is 14 bits, split across write bytes 1 and 2:
//   Byte 1 bits [5:0] = PLL[13:8]
//   Byte 2 bits [7:0] = PLL[7:0]
// ---------------------------------------------------------------------------
#define TEA5767_IF_HZ       225000UL
#define TEA5767_FREF_HZ     32768UL

// ---------------------------------------------------------------------------
// Write-byte bit definitions
// ---------------------------------------------------------------------------

// ---- Byte 1 ----------------------------------------------------------------
// Bit 7: MUTE  – 1 = both channels muted
// Bit 6: SM    – 1 = search mode
// Bits 5:0: PLL[13:8]
#define B1_MUTE             (1u << 7)
#define B1_SM               (1u << 6)
// PLL[13:8] sits in bits [5:0]; mask applied when building the byte.

// ---- Byte 3 ----------------------------------------------------------------
// Bit 7: SUD   – 1 = search up
// Bits 6:5: SSL[1:0] – search stop level (01=low, 10=mid, 11=high)
// Bit 4: HLSI  – 1 = high-side LO injection (used for US/Europe band)
// Bit 3: MS    – 1 = force mono
// Bit 2: MR    – 1 = mute right channel
// Bit 1: ML    – 1 = mute left channel
// Bit 0: SWP1  – software port 1 value
#define B3_HLSI             (1u << 4)   // high-side injection

// ---- Byte 4 ----------------------------------------------------------------
// Bit 7: SWP2  – software port 2 value
// Bit 6: STBY  – 1 = standby mode
// Bit 5: BL    – 1 = Japanese band (76–91 MHz); 0 = US/Europe (87.5–108 MHz)
// Bit 4: XTAL  – 1 = 32.768 kHz crystal; 0 = 13 MHz crystal
// Bit 3: SMUTE – 1 = soft mute ON
// Bit 2: HCC   – 1 = high cut control ON
// Bit 1: SNC   – 1 = stereo noise cancelling ON
// Bit 0: SI    – 1 = SWPORT1 outputs ready flag
#define B4_XTAL             (1u << 4)   // 32.768 kHz crystal selected
#define B4_HCC              (1u << 2)   // high cut control (improves weak-signal mono quality)
#define B4_SNC              (1u << 1)   // stereo noise cancelling

// ---- Byte 5 ----------------------------------------------------------------
// Bit 7: PLLREF – 1 = 6.5 MHz PLL ref; 0 = use XTAL
// Bit 6: DTC    – 1 = 75 µs de-emphasis (US standard); 0 = 50 µs (Europe)
// Bits 5:0: don't care
#define B5_DTC              (1u << 6)   // 75 µs de-emphasis (US/Canada standard)

// ---------------------------------------------------------------------------
// Read-byte bit definitions
// ---------------------------------------------------------------------------

// ---- Read byte 1 -----------------------------------------------------------
// Bit 7: RF  (Ready Flag)      – 1 = station found / band limit reached
// Bit 6: BLF (Band Limit Flag) – 1 = band limit reached during search
// Bits 5:0: PLL[13:8] (read-back of current PLL word high bits)
#define RB1_RF              (1u << 7)
#define RB1_BLF             (1u << 6)
#define RB1_PLL_HI_MASK     0x3Fu

// ---- Read byte 3 -----------------------------------------------------------
// Bit 7: STEREO  – 1 = stereo reception
// Bits 6:0: IF counter result (7 bits)
#define RB3_STEREO          (1u << 7)
#define RB3_IF_MASK         0x7Fu

// ---- Read byte 4 -----------------------------------------------------------
// Bits 7:4: LEV[3:0] – signal level ADC (0–15, higher = stronger)
// Bits 3:1: CI[3:1]  – chip identification (should read as 0)
#define RB4_LEV_SHIFT       4
#define RB4_LEV_MASK        0xF0u

// Minimum signal level considered a "valid" station (out of 15).
// Level 5 corresponds to SSL=01 (low search stop threshold) in the datasheet.
#define TEA5767_MIN_SIGNAL_LEVEL  3u

// ---------------------------------------------------------------------------
// Private helpers: low-level I2C transactions
//
// The TEA5767 has no internal address pointer. A write always delivers all
// 5 bytes in order; a read always returns 5 bytes in order.
// ---------------------------------------------------------------------------

/**
 * i2c_write_bytes
 *
 * Writes exactly `len` bytes from `buf[]` to the TEA5767 over I2C.
 * Uses the CC3200 burst-send API (driverlib i2c.h).
 *
 * @return  TEA5767_OK or TEA5767_ERR_I2C_WRITE
 */
static int i2c_write_bytes(const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (len == 0) return TEA5767_OK;

    MAP_I2CMasterSlaveAddrSet(I2CA0_BASE, TEA5767_I2C_ADDR, false /* write */);

    if (len == 1) {
        MAP_I2CMasterDataPut(I2CA0_BASE, buf[0]);
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_SINGLE_SEND);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return TEA5767_ERR_I2C_WRITE;
        return TEA5767_OK;
    }

    // Multi-byte: START + first byte
    MAP_I2CMasterDataPut(I2CA0_BASE, buf[0]);
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_SEND_START);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return TEA5767_ERR_I2C_WRITE;

    // Middle bytes
    for (i = 1; i < len - 1; i++) {
        MAP_I2CMasterDataPut(I2CA0_BASE, buf[i]);
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_SEND_CONT);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return TEA5767_ERR_I2C_WRITE;
    }

    // Last byte + STOP
    MAP_I2CMasterDataPut(I2CA0_BASE, buf[len - 1]);
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_SEND_FINISH);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return TEA5767_ERR_I2C_WRITE;

    return TEA5767_OK;
}

/**
 * i2c_read_bytes
 *
 * Reads exactly `len` bytes from the TEA5767 into `buf[]`.
 * Uses the CC3200 burst-receive API (driverlib i2c.h).
 *
 * @return  TEA5767_OK or TEA5767_ERR_I2C_READ
 */
static int i2c_read_bytes(uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (len == 0) return TEA5767_OK;

    MAP_I2CMasterSlaveAddrSet(I2CA0_BASE, TEA5767_I2C_ADDR, true /* read */);

    if (len == 1) {
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_SINGLE_RECEIVE);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return TEA5767_ERR_I2C_READ;
        buf[0] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);
        return TEA5767_OK;
    }

    // Multi-byte: START + receive first byte
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_START);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return TEA5767_ERR_I2C_READ;
    buf[0] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);

    // Middle bytes
    for (i = 1; i < len - 1; i++) {
        MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_CONT);
        while (MAP_I2CMasterBusy(I2CA0_BASE));
        if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
            return TEA5767_ERR_I2C_READ;
        buf[i] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);
    }

    // Last byte: NACK + STOP
    MAP_I2CMasterControl(I2CA0_BASE, I2C_MASTER_CMD_BURST_RECEIVE_FINISH);
    while (MAP_I2CMasterBusy(I2CA0_BASE));
    if (MAP_I2CMasterErr(I2CA0_BASE) != I2C_MASTER_ERR_NONE)
        return TEA5767_ERR_I2C_READ;
    buf[len - 1] = (uint8_t)MAP_I2CMasterDataGet(I2CA0_BASE);

    return TEA5767_OK;
}

// ---------------------------------------------------------------------------
// Public API implementation
// ---------------------------------------------------------------------------

int TEA5767_Init(void)
{
    int ret;

    // ---- 1. Configure I2C pins on the CC3200 --------------------------------
    // PIN_01 = I2C SCL (Mode 1),  PIN_02 = I2C SDA (Mode 1)
    // External 4.7k pull-ups to 3.3V required on both lines.
    MAP_PinTypeI2C(PIN_01, PIN_MODE_1);
    MAP_PinTypeI2C(PIN_02, PIN_MODE_1);

    // ---- 2. Enable and initialise the I2C master peripheral -----------------
    MAP_PRCMPeripheralClkEnable(PRCM_I2CA0, PRCM_RUN_MODE_CLK);
    while (!MAP_PRCMPeripheralStatusGet(PRCM_I2CA0));

    // false = 100 kHz standard mode (TEA5767 supports up to 400 kHz)
    MAP_I2CMasterInitExpClk(I2CA0_BASE, SYS_CLK_HZ, false);

    // ---- 3. Send a default 5-byte configuration frame ----------------------
    // Wake the chip from power-on state with sensible defaults:
    //   Byte 1: MUTE=0, SM=0, PLL[13:8]=0  (frequency set properly by TuneFrequency)
    //   Byte 2: PLL[7:0]=0
    //   Byte 3: HLSI=1 (high-side injection for US/Europe), MS=0 (stereo), MR=0, ML=0
    //   Byte 4: XTAL=1 (32.768 kHz crystal), HCC=1, SNC=1, STBY=0
    //   Byte 5: DTC=1 (75 µs de-emphasis, US standard), PLLREF=0
    uint8_t init_frame[5] = {
        0x00u,                              // Byte 1: unmuted, no search, PLL hi = 0
        0x00u,                              // Byte 2: PLL lo = 0
        B3_HLSI,                            // Byte 3: high-side injection, stereo, not muted
        (B4_XTAL | B4_HCC | B4_SNC),       // Byte 4: 32.768 kHz xtal, HCC & SNC on
        B5_DTC                              // Byte 5: 75 µs de-emphasis
    };

    ret = i2c_write_bytes(init_frame, 5);
    if (ret != TEA5767_OK) {
        UART_PRINT("[TEA5767] Init: configuration write failed (%d)\n\r", ret);
        return ret;
    }

    // Allow the PLL and oscillator to stabilise
    MAP_UtilsDelay(SYS_CLK_HZ / 3 / 100);  // ~10 ms

    UART_PRINT("[TEA5767] Init complete.\n\r");
    return TEA5767_OK;
}

// ---------------------------------------------------------------------------

int TEA5767_TuneFrequency(float freq_mhz)
{
    int     ret;
    uint8_t tx[5];
    uint8_t rx[5];
    int     poll;

    // ---- 1. Validate frequency ---------------------------------------------
    if (freq_mhz < TEA5767_BAND_MIN_MHZ || freq_mhz > TEA5767_BAND_MAX_MHZ) {
        UART_PRINT("[TEA5767] Frequency %.1f MHz out of range (87.5–108.0 MHz)\n\r",
                   (double)freq_mhz);
        return TEA5767_ERR_FREQ;
    }

    // ---- 2. Calculate 14-bit PLL word (N) ----------------------------------
    // Datasheet §8.4, high-side injection formula:
    //   N = 4 * (fRF_Hz + fIF_Hz) / fref_Hz
    //   fIF  = 225,000 Hz
    //   fref = 32,768  Hz  (32.768 kHz crystal)
    uint32_t frf_hz = (uint32_t)(freq_mhz * 1000000.0f + 0.5f);
    uint16_t pll    = (uint16_t)(4UL * (frf_hz + TEA5767_IF_HZ) / TEA5767_FREF_HZ);

    // ---- 3. Build the 5-byte write frame -----------------------------------
    // Byte 1: MUTE=0, SM=0, PLL[13:8]
    tx[0] = (uint8_t)(pll >> 8) & RB1_PLL_HI_MASK;   // upper 6 bits of PLL word

    // Byte 2: PLL[7:0]
    tx[1] = (uint8_t)(pll & 0xFFu);

    // Byte 3: HLSI=1 (high-side injection), MS=0 (stereo), MR=0, ML=0, SWP1=0
    tx[2] = B3_HLSI;

    // Byte 4: XTAL=1 (32.768 kHz), HCC=1, SNC=1, STBY=0, BL=0 (US/Europe)
    tx[3] = (B4_XTAL | B4_HCC | B4_SNC);

    // Byte 5: DTC=1 (75 µs de-emphasis), PLLREF=0
    tx[4] = B5_DTC;

    UART_PRINT("[TEA5767] Tuning to %.1f MHz (PLL=0x%04X)...\n\r",
               (double)freq_mhz, (unsigned)pll);

    ret = i2c_write_bytes(tx, 5);
    if (ret != TEA5767_OK) {
        UART_PRINT("[TEA5767] Tune write failed (%d)\n\r", ret);
        return ret;
    }

    // ---- 4. Poll Ready Flag (RF) in read byte 1 ----------------------------
    // Datasheet §8.5, Table 18: RF=1 when a station has been found or
    // the band limit has been reached. We poll until RF=1 or timeout.
    for (poll = 0; poll < RF_POLL_MAX; poll++) {
        MAP_UtilsDelay(SYS_CLK_HZ / 3 / 1000);    // ~1 ms per iteration

        ret = i2c_read_bytes(rx, 5);
        if (ret != TEA5767_OK) {
            UART_PRINT("[TEA5767] Read failed during RF poll (%d)\n\r", ret);
            return ret;
        }

        if (rx[0] & RB1_RF) {
            break;  // PLL locked / station found
        }
    }

    if (poll >= RF_POLL_MAX) {
        UART_PRINT("[TEA5767] Timeout waiting for RF flag after %d ms\n\r",
                   RF_POLL_MAX);
        return TEA5767_ERR_TIMEOUT;
    }

    // ---- 5. Parse and report status ----------------------------------------
    //
    // Read byte 1: RF (bit 7), BLF (bit 6), PLL[13:8] (bits 5:0)
    // Read byte 2: PLL[7:0]
    // Read byte 3: STEREO (bit 7), IF counter (bits 6:0)
    // Read byte 4: LEV[3:0] (bits 7:4), CI[3:1] (bits 3:1)
    // Read byte 5: reserved (all 0)

    uint8_t  rf_flag  = (rx[0] & RB1_RF)     ? 1 : 0;
    uint8_t  blf_flag = (rx[0] & RB1_BLF)    ? 1 : 0;
    uint8_t  stereo   = (rx[2] & RB3_STEREO)  ? 1 : 0;
    uint8_t  if_cnt   =  rx[2] & RB3_IF_MASK;
    uint8_t  level    = (uint8_t)((rx[3] & RB4_LEV_MASK) >> RB4_LEV_SHIFT);  // 0–15

    // Reconstruct confirmed tuned frequency from read-back PLL word
    uint16_t pll_rb   = (uint16_t)(((rx[0] & RB1_PLL_HI_MASK) << 8) | rx[1]);
    float    actual_mhz = ((float)pll_rb * (float)TEA5767_FREF_HZ / 4.0f
                           - (float)TEA5767_IF_HZ) / 1000000.0f;

    UART_PRINT("--------------------------------------------------\n\r");
    UART_PRINT("[TEA5767] Tune complete after %d ms\n\r", poll);
    UART_PRINT("  Requested  : %.1f MHz\n\r",  (double)freq_mhz);
    UART_PRINT("  Confirmed  : %.1f MHz\n\r",  (double)actual_mhz);
    UART_PRINT("  RF Flag    : %s\n\r",  rf_flag  ? "YES (PLL locked)"        : "NO");
    UART_PRINT("  BLF Flag   : %s\n\r",  blf_flag ? "YES (band limit reached)" : "NO");
    UART_PRINT("  Signal Lvl : %u / 15 (0=min, 15=max)\n\r", (unsigned)level);
    UART_PRINT("  IF Counter : %u\n\r",  (unsigned)if_cnt);
    UART_PRINT("  Stereo     : %s\n\r",  stereo   ? "YES" : "NO (mono)");
    UART_PRINT("--------------------------------------------------\n\r");

    // ---- 6. Return result --------------------------------------------------

    // If the band limit was hit, the PLL locked at the edge of the band, not
    // on the requested frequency. This is only possible in search mode (not
    // triggered here), so BLF should never be set in normal preset tuning.
    if (blf_flag) {
        UART_PRINT("[TEA5767] WARNING: band limit reached — unexpected in preset mode.\n\r");
    }

    if (level < TEA5767_MIN_SIGNAL_LEVEL) {
        UART_PRINT("[TEA5767] WARNING: signal level very low (%u/15). "
                   "Check antenna connection.\n\r", (unsigned)level);
        return TEA5767_ERR_NO_SIGNAL;
    }

    UART_PRINT("[TEA5767] Playback active on %.1f MHz.\n\r", (double)freq_mhz);
    return TEA5767_OK;
}
