#ifndef DRV_ADS131M02_H
#define DRV_ADS131M02_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32h5xx_hal.h"

/**
 * ADS131M02 - 24-bit, 2-channel, simultaneous-sampling delta-sigma ADC
 *
 * SPI frame format (24-bit word size, default):
 *   Command frame:  [CMD_WORD] [0x000000] [0x000000] [CRC_WORD]
 *   Response frame:  [STATUS]   [CH0_DATA] [CH1_DATA] [CRC_WORD]
 *
 * Each SPI transaction is exactly 4 words (4 x 24 bits = 96 bits = 12 bytes).
 * DRDY goes low when new data is available.
 * CS must be held low during the entire frame transaction.
 */

/* Register addresses */
#define ADS131M02_REG_ID            0x00
#define ADS131M02_REG_STATUS        0x01
#define ADS131M02_REG_MODE          0x02
#define ADS131M02_REG_CLOCK         0x03
#define ADS131M02_REG_GAIN          0x04
#define ADS131M02_REG_CFG           0x06
#define ADS131M02_REG_THRSHLD_MSB   0x07
#define ADS131M02_REG_THRSHLD_LSB   0x08
#define ADS131M02_REG_CH0_CFG       0x09
#define ADS131M02_REG_CH0_OCAL_MSB  0x0A
#define ADS131M02_REG_CH0_OCAL_LSB  0x0B
#define ADS131M02_REG_CH0_GCAL_MSB  0x0C
#define ADS131M02_REG_CH0_GCAL_LSB  0x0D
#define ADS131M02_REG_CH1_CFG       0x0E
#define ADS131M02_REG_CH1_OCAL_MSB  0x0F
#define ADS131M02_REG_CH1_OCAL_LSB  0x10
#define ADS131M02_REG_CH1_GCAL_MSB  0x11
#define ADS131M02_REG_CH1_GCAL_LSB  0x12
#define ADS131M02_REG_REGMAP_CRC    0x3E

/* SPI commands (upper 16 bits of 24-bit command word) */
#define ADS131M02_CMD_NULL          0x0000
#define ADS131M02_CMD_RESET         0x0011
#define ADS131M02_CMD_STANDBY       0x0022
#define ADS131M02_CMD_WAKEUP        0x0033
#define ADS131M02_CMD_LOCK          0x0555
#define ADS131M02_CMD_UNLOCK        0x0655
#define ADS131M02_CMD_RREG_BASE     0xA000  /* RREG: 0xA000 | (addr << 7) */
#define ADS131M02_CMD_WREG_BASE     0x6000  /* WREG: 0x6000 | (addr << 7) */

/* Expected device ID (upper byte of ID register) */
#define ADS131M02_DEVICE_ID         0x22

/* CLOCK register bits */
#define ADS131M02_CLK_CH0_EN        (1 << 8)
#define ADS131M02_CLK_CH1_EN        (1 << 9)
/*
 * OSR field: bits [2:0] of CLOCK register.
 * Encoding per ADS131M02 datasheet Table 8-12:
 *   0x00 → OSR=128  → 64 kSPS  @ 8.192 MHz CLKIN (HR mode)
 *   0x01 → OSR=256  → 32 kSPS
 *   0x02 → OSR=512  → 16 kSPS
 *   0x03 → OSR=1024 →  8 kSPS
 *   0x04 → OSR=2048 →  4 kSPS
 *   0x05 → OSR=4096 →  2 kSPS
 *   0x06 → OSR=8192 →  1 kSPS
 *   0x07 → OSR=16384→ 500 SPS  (HR only)
 *
 * Previous values (0x04–0x09) were off by +4 — they encoded OSR=2048–16384
 * instead of OSR=128–4096.  Verified empirically: 1 kHz sine wave produced
 * This comment block was corrected after reviewing the ADS131M02 data-rate
 * table: OSR=128 in HR mode maps to 32 kSPS, not 64 kSPS.
 */
/* Correct HR-mode data rates at 8.192 MHz CLKIN: OSR=128 maps to 32 kSPS. */
#define ADS131M02_CLK_OSR_128       0x0000  /* 32 kSPS @ 8.192 MHz, HR */
#define ADS131M02_CLK_OSR_256       0x0001  /* 16 kSPS */
#define ADS131M02_CLK_OSR_512       0x0002  /*  8 kSPS */
#define ADS131M02_CLK_OSR_1024      0x0003  /*  4 kSPS */
#define ADS131M02_CLK_OSR_2048      0x0004  /*  2 kSPS */
#define ADS131M02_CLK_OSR_4096      0x0005  /*  1 kSPS */
#define ADS131M02_CLK_PWR_HR        (0x03 << 4)  /* High-resolution mode */
#define ADS131M02_CLK_PWR_LP        (0x02 << 4)  /* Low-power mode */
#define ADS131M02_CLK_PWR_VLP       (0x01 << 4)  /* Very low-power mode */

/* GAIN register: PGA gain per channel */
#define ADS131M02_GAIN_1            0x00
#define ADS131M02_GAIN_2            0x01
#define ADS131M02_GAIN_4            0x02
#define ADS131M02_GAIN_8            0x03
#define ADS131M02_GAIN_16           0x04
#define ADS131M02_GAIN_32           0x05
#define ADS131M02_GAIN_64           0x06
#define ADS131M02_GAIN_128          0x07

/* ========================================================================= */

/** ADC sample data for both channels */
typedef struct {
    int32_t ch0;    /* 24-bit signed, sign-extended to 32-bit */
    int32_t ch1;    /* 24-bit signed, sign-extended to 32-bit */
    uint16_t status; /* Status word from frame */
    bool valid;     /* CRC check passed */
} ads131m02_sample_t;

/** Callback type for DRDY interrupt — called with new sample data */
typedef void (*ads131m02_drdy_cb_t)(const ads131m02_sample_t *sample);

/**
 * Initialize the ADS131M02.
 * - Reset device
 * - Verify device ID
 * - Configure: both channels enabled, OSR=128 (32kSPS), gain=1, HR mode
 * - Enable CRC
 *
 * @return 0 on success, negative on error (e.g., ID mismatch)
 */
int ads131m02_init(void);

/**
 * Read the device ID register.
 * @return Device ID value, or negative on error
 */
int ads131m02_read_id(void);

/**
 * Read a single register.
 * @param addr  Register address
 * @param value Output: register value
 * @return 0 on success
 */
int ads131m02_read_reg(uint8_t addr, uint16_t *value);

/**
 * Write a single register.
 * @param addr  Register address
 * @param value Register value to write
 * @return 0 on success
 */
int ads131m02_write_reg(uint8_t addr, uint16_t value);

/**
 * Read one sample frame (blocking, polls DRDY or uses current data).
 * @param sample  Output sample data
 * @return 0 on success
 */
int ads131m02_read_sample(ads131m02_sample_t *sample);

/**
 * Start continuous acquisition with DRDY interrupt.
 * Each time DRDY fires, the driver reads a frame via SPI and calls the
 * registered callback.
 *
 * @param callback  Function called from ISR context with new sample
 */
void ads131m02_start_continuous(ads131m02_drdy_cb_t callback);

/**
 * Stop continuous acquisition.
 */
void ads131m02_stop_continuous(void);

/**
 * Set the oversampling ratio (determines sample rate).
 * @param osr  One of ADS131M02_CLK_OSR_* values
 * @return 0 on success
 */
int ads131m02_set_osr(uint8_t osr);

/**
 * Set the PGA gain for a channel.
 * @param channel  0 or 1
 * @param gain     One of ADS131M02_GAIN_* values
 * @return 0 on success
 */
int ads131m02_set_gain(uint8_t channel, uint8_t gain);

/**
 * Convert raw 24-bit ADC code to voltage.
 * @param code  Signed 24-bit value (sign-extended to int32_t)
 * @param gain  PGA gain setting
 * @return Voltage in volts
 */
float ads131m02_code_to_voltage(int32_t code, uint8_t gain);

/**
 * Perform a low-level SPI frame transaction.
 * Sends 4 words, receives 4 words (24-bit each).
 *
 * @param tx  Transmit buffer (4 x uint32_t, only lower 24 bits used)
 * @param rx  Receive buffer (4 x uint32_t, lower 24 bits valid)
 * @return 0 on success
 */
int ads131m02_spi_transfer(const uint32_t tx[4], uint32_t rx[4]);

/**
 * DRDY ISR handler — called from EXTI11 falling edge callback.
 * Starts a DMA read of the ADC frame.
 */
void ads131m02_drdy_isr_handler(void);

/**
 * SPI2 TX/RX DMA complete callback dispatcher.
 * Called from HAL_SPI_TxRxCpltCallback when hspi == &hspi2.
 */
void HAL_SPI_TxRxCpltCallback_ADC(SPI_HandleTypeDef *hspi);

#endif /* DRV_ADS131M02_H */
