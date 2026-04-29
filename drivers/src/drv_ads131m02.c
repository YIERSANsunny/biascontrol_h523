#include "drv_ads131m02.h"
#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "stm32h5xx_hal.h"

#include <stddef.h>
#include <stdio.h>

/*
 * ADS131M02 driver implementation.
 *
 * SPI frame format (24-bit word mode, 4 words per frame):
 *   TX: [COMMAND] [0x000000] [0x000000] [CRC]
 *   RX: [STATUS]  [CH0_DATA] [CH1_DATA] [CRC]
 *
 * Word size is 24 bits. SPI uses 8-bit granularity,
 * so each word is 3 bytes (MSB first). Total frame: 12 bytes.
 *
 * SPI Mode 1: CPOL=0, CPHA=1.
 * CS must remain low during the entire 4-word frame.
 */

/* DRDY callback for continuous mode */
static ads131m02_drdy_cb_t drdy_callback = NULL;

/* DMA transfer buffers and completion flag */
static uint8_t adc_tx_buf[12];
static uint8_t adc_rx_buf[12];
static volatile bool spi2_xfer_done = true;

/* Last parsed sample (filled by DMA callback) */
static volatile ads131m02_sample_t last_sample;

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */

/** Build a RREG command word: 0xA000 | (addr << 7) */
static uint32_t ads131m02_rreg_cmd(uint8_t addr)
{
    return (uint32_t)(ADS131M02_CMD_RREG_BASE | ((uint16_t)addr << 7)) << 8;
}

/** Build a WREG command word: 0x6000 | (addr << 7) */
static uint32_t ads131m02_wreg_cmd(uint8_t addr)
{
    return (uint32_t)(ADS131M02_CMD_WREG_BASE | ((uint16_t)addr << 7)) << 8;
}

/** Sign-extend a 24-bit value to 32-bit. */
static int32_t sign_extend_24(uint32_t val)
{
    if (val & 0x00800000) {
        return (int32_t)(val | 0xFF000000);
    }
    return (int32_t)val;
}

/** Pack 4x 24-bit words into 12-byte buffer (MSB first) */
static void pack_frame(const uint32_t words[4], uint8_t buf[12])
{
    for (int i = 0; i < 4; i++) {
        buf[i * 3 + 0] = (uint8_t)(words[i] >> 16);
        buf[i * 3 + 1] = (uint8_t)(words[i] >> 8);
        buf[i * 3 + 2] = (uint8_t)(words[i]);
    }
}

/** Unpack 12-byte buffer into 4x 24-bit words */
static void unpack_frame(const uint8_t buf[12], uint32_t words[4])
{
    for (int i = 0; i < 4; i++) {
        words[i] = ((uint32_t)buf[i * 3 + 0] << 16) |
                   ((uint32_t)buf[i * 3 + 1] << 8) |
                   ((uint32_t)buf[i * 3 + 2]);
    }
}

/* ========================================================================= */
/*  HAL callbacks                                                            */
/* ========================================================================= */

/**
 * SPI2 TX/RX DMA complete callback.
 * Called by HAL from GPDMA1_Channel2 (SPI2_RX) ISR.
 * Parses the received frame and dispatches to user callback.
 */
void HAL_SPI_TxRxCpltCallback_ADC(SPI_HandleTypeDef *hspi)
{
    if (hspi != &hspi2) {
        return;
    }

    board_adc_cs_high();
    spi2_xfer_done = true;

    /* Parse received frame */
    uint32_t rx[4];
    unpack_frame(adc_rx_buf, rx);

    ads131m02_sample_t sample;
    sample.status = (uint16_t)(rx[0] >> 8);
    sample.ch0 = sign_extend_24(rx[1]);
    sample.ch1 = sign_extend_24(rx[2]);
    sample.valid = true; /* TODO: verify CRC from rx[3] */

    last_sample = sample;

    /* Dispatch to user callback (runs in ISR context) */
    if (drdy_callback != NULL) {
        drdy_callback(&sample);
    }
}

/* ========================================================================= */
/*  Low-level SPI (blocking, for init/config)                                */
/* ========================================================================= */

int ads131m02_spi_transfer(const uint32_t tx[4], uint32_t rx[4])
{
    uint8_t tx_buf[12];
    uint8_t rx_buf[12];

    pack_frame(tx, tx_buf);

    board_adc_cs_low();

    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(
        &hspi2, tx_buf, rx_buf, 12, HAL_MAX_DELAY);

    board_adc_cs_high();

    if (status != HAL_OK) {
        return -1;
    }

    unpack_frame(rx_buf, rx);
    return 0;
}

/**
 * Start a DMA-based SPI2 transfer (non-blocking).
 * Used by the DRDY ISR path for maximum throughput.
 */
static int ads131m02_spi_transfer_dma(const uint32_t tx[4])
{
    pack_frame(tx, adc_tx_buf);

    spi2_xfer_done = false;
    board_adc_cs_low();

    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive_DMA(
        &hspi2, adc_tx_buf, adc_rx_buf, 12);

    if (status != HAL_OK) {
        board_adc_cs_high();
        spi2_xfer_done = true;
        return -1;
    }

    return 0;
}

/* ========================================================================= */
/*  Register access                                                          */
/* ========================================================================= */

int ads131m02_read_reg(uint8_t addr, uint16_t *value)
{
    uint32_t tx[4] = {ads131m02_rreg_cmd(addr), 0, 0, 0};
    uint32_t rx[4];

    /* First transfer sends the RREG command */
    int ret = ads131m02_spi_transfer(tx, rx);
    if (ret != 0) {
        return ret;
    }

    /* The register data comes back in the NEXT frame's response */
    uint32_t tx_null[4] = {0, 0, 0, 0};
    ret = ads131m02_spi_transfer(tx_null, rx);
    if (ret != 0) {
        return ret;
    }

    /* Register value is in bits [23:8] of the status/response word */
    *value = (uint16_t)((rx[0] >> 8) & 0xFFFF);
    return 0;
}

int ads131m02_write_reg(uint8_t addr, uint16_t value)
{
    uint32_t tx[4] = {
        ads131m02_wreg_cmd(addr),
        (uint32_t)value << 8, /* Data word: value in upper 16 bits of 24 */
        0,
        0
    };
    uint32_t rx[4];

    int ret = ads131m02_spi_transfer(tx, rx);
    if (ret != 0) {
        return ret;
    }

    /* Read back to verify (response comes in next frame) */
    uint32_t tx_null[4] = {0, 0, 0, 0};
    ret = ads131m02_spi_transfer(tx_null, rx);
    (void)rx; /* Could verify acknowledgment here */

    return ret;
}

/* ========================================================================= */
/*  Init and configuration                                                   */
/* ========================================================================= */

int ads131m02_read_id(void)
{
    uint16_t id;
    int ret = ads131m02_read_reg(ADS131M02_REG_ID, &id);
    if (ret != 0) {
        return -1;
    }
    return (int)(id >> 8); /* Upper byte is device ID */
}

int ads131m02_init(void)
{
    /* Hardware reset via /SYNC/RESET pin */
    board_adc_sync_rst_assert();
    board_delay_ms(1);
    board_adc_sync_rst_release();
    board_delay_ms(10); /* Wait for device to come out of reset */

    /* Software reset */
    uint32_t tx_reset[4] = {(uint32_t)ADS131M02_CMD_RESET << 8, 0, 0, 0};
    uint32_t rx[4];
    int ret = ads131m02_spi_transfer(tx_reset, rx);
    if (ret != 0) {
        printf("[adc] SPI transfer failed\r\n");
        return ret;
    }

    board_delay_ms(5);

    /* Verify device ID */
    int id = ads131m02_read_id();
    if (id < 0) {
        printf("[adc] read ID failed\r\n");
        return -1;
    }
    if (id != ADS131M02_DEVICE_ID) {
        printf("[adc] ID mismatch: got 0x%02X, expected 0x%02X\r\n",
               id, ADS131M02_DEVICE_ID);
        return -2;
    }

    printf("[adc] ID=0x%02X ok\r\n", id);

    /* Configure CLOCK register:
     * - Both channels enabled
     * - High-resolution power mode
     * - OSR = 128 → 64 kSPS at 8.192 MHz CLKIN (code 0x00)
     *   Note: previously used ADS131M02_CLK_OSR_256 (old value 0x05 = code 5
     *   = OSR 4096 = 2 kSPS), which was wrong.  Corrected to OSR_128 (0x00)
     *   after empirical verification with a 1 kHz sine wave.
     */
    /* Correct data-rate mapping: OSR=128 in HR mode gives 32 kSPS at 8.192 MHz CLKIN. */
    uint16_t clock_val = ADS131M02_CLK_CH0_EN | ADS131M02_CLK_CH1_EN |
                         ADS131M02_CLK_PWR_HR | ADS131M02_CLK_OSR_128;
    ret = ads131m02_write_reg(ADS131M02_REG_CLOCK, clock_val);
    if (ret != 0) {
        return ret;
    }

    /* Set gain = 1 for both channels */
    uint16_t gain_val = (ADS131M02_GAIN_1 << 8) | ADS131M02_GAIN_1;
    ret = ads131m02_write_reg(ADS131M02_REG_GAIN, gain_val);
    if (ret != 0) {
        return ret;
    }

    printf("[adc] configured: OSR=128, GAIN=1\r\n");
    return 0;
}

int ads131m02_set_osr(uint8_t osr)
{
    uint16_t clock_val;
    int ret = ads131m02_read_reg(ADS131M02_REG_CLOCK, &clock_val);
    if (ret != 0) {
        return ret;
    }

    /* Clear OSR bits [3:0] and set new value */
    clock_val = (clock_val & 0xFFF0) | (osr & 0x0F);
    return ads131m02_write_reg(ADS131M02_REG_CLOCK, clock_val);
}

int ads131m02_set_gain(uint8_t channel, uint8_t gain)
{
    uint16_t gain_val;
    int ret = ads131m02_read_reg(ADS131M02_REG_GAIN, &gain_val);
    if (ret != 0) {
        return ret;
    }

    if (channel == 0) {
        gain_val = (gain_val & 0xFF00) | (gain & 0x07);
    } else if (channel == 1) {
        gain_val = (gain_val & 0x00FF) | ((uint16_t)(gain & 0x07) << 8);
    } else {
        return -1;
    }

    return ads131m02_write_reg(ADS131M02_REG_GAIN, gain_val);
}

/* ========================================================================= */
/*  Data acquisition                                                         */
/* ========================================================================= */

int ads131m02_read_sample(ads131m02_sample_t *sample)
{
    uint32_t tx[4] = {0, 0, 0, 0}; /* NULL command */
    uint32_t rx[4];

    int ret = ads131m02_spi_transfer(tx, rx);
    if (ret != 0) {
        return ret;
    }

    sample->status = (uint16_t)(rx[0] >> 8);
    sample->ch0 = sign_extend_24(rx[1]);
    sample->ch1 = sign_extend_24(rx[2]);
    sample->valid = true; /* TODO: verify CRC from rx[3] */

    return 0;
}

void ads131m02_start_continuous(ads131m02_drdy_cb_t callback)
{
    drdy_callback = callback;
    /* Enable EXTI interrupt on PA11 (DRDY) */
    HAL_NVIC_EnableIRQ(ADC_DRDY_EXTI_IRQn);
}

void ads131m02_stop_continuous(void)
{
    HAL_NVIC_DisableIRQ(ADC_DRDY_EXTI_IRQn);
    drdy_callback = NULL;
}

/**
 * Called from HAL_GPIO_EXTI_Falling_Callback when DRDY goes low.
 * Starts a DMA read of the ADC frame. The DMA complete callback
 * (HAL_SPI_TxRxCpltCallback_ADC) will parse and dispatch.
 */
void ads131m02_drdy_isr_handler(void)
{
    if (drdy_callback == NULL) {
        return;
    }

    /* Start non-blocking DMA transfer */
    static const uint32_t tx_null[4] = {0, 0, 0, 0};
    ads131m02_spi_transfer_dma(tx_null);
}

/* ========================================================================= */
/*  Utility                                                                  */
/* ========================================================================= */

float ads131m02_code_to_voltage(int32_t code, uint8_t gain)
{
    /*
     * Full-scale range: +/-1.2V / gain (1.2V internal reference)
     * 24-bit code range: [-8388608, +8388607]
     * V = code * (1.2 / gain) / 8388608
     */
    static const float gain_table[] = {1, 2, 4, 8, 16, 32, 64, 128};
    float g = (gain < 8) ? gain_table[gain] : 1.0f;
    return (float)code * 1.2f / (g * 8388608.0f);
}
