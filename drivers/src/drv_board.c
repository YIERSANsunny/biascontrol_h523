#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "stm32h5xx_hal.h"

#include <stdio.h>
#include <string.h>

/*
 * Board-level initialization and utility functions.
 *
 * All GPIO pins are initialized by CubeMX in MX_GPIO_Init().
 * This module provides a thin abstraction layer over HAL_GPIO calls,
 * using the pin/port macros defined in main.h (CubeMX generated).
 */

/* ========================================================================= */
/*  USART printf redirect — DMA TX                                           */
/* ========================================================================= */

/*
 * _write() — newlib/picolibc syscall backing printf().
 *
 * Blocking HAL_UART_Transmit is intentional here.  A DMA-based _write()
 * requires the UART TC interrupt chain (GPDMA TCF → UART TC → callback) to
 * fire reliably between every printf() call.  On STM32H5 GPDMA this chain
 * can stall (same root cause as the SPI1 DMA callback issue documented in
 * CLAUDE.md), causing subsequent writes to be silently discarded.
 *
 * Blocking TX is safe because the only latency-sensitive path (ADC sample
 * acquisition) now performs all its printf() calls *after* the full sample
 * burst is collected, never interleaved with DRDY polling.
 */
void board_uart_tx_cplt(void)
{
    /* No-op: retained so drv_callbacks.c compiles without change. */
}

int _write(int fd, char *ptr, int len)
{
    (void)fd;
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
    return len;
}

/* ========================================================================= */
/*  Board init — called once at startup                                      */
/* ========================================================================= */

void board_init(void)
{
    /*
     * CubeMX main.c already calls:
     *   HAL_Init(), SystemClock_Config(), MX_GPIO_Init(), MX_GPDMA1_Init(),
     *   MX_SPI1_Init(), MX_SPI2_Init(), MX_TIM6_Init(), MX_USART1_UART_Init()
     *
     * This function ensures default pin states after CubeMX init.
     */

    /* Ensure default pin states (CubeMX already sets initial levels,
     * but be explicit for safety) */
    board_dac_cs_high();          /* DAC deselected */
    board_dac_clr_release();      /* DAC CLR inactive (high) */
    board_adc_cs_high();          /* ADC deselected */
    board_adc_sync_rst_release(); /* ADC /SYNC/RESET inactive (high) */
    board_led_off();

    printf("[board] init ok, SYSCLK=%lu MHz\r\n",
           HAL_RCC_GetSysClockFreq() / 1000000UL);
}

/* ========================================================================= */
/*  LED                                                                      */
/* ========================================================================= */

void board_led_on(void)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
}

void board_led_off(void)
{
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
}

void board_led_toggle(void)
{
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
}

/* ========================================================================= */
/*  Delay                                                                    */
/* ========================================================================= */

void board_delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

uint32_t board_get_tick_ms(void)
{
    return HAL_GetTick();
}

/* ========================================================================= */
/*  DAC chip-select and control pins                                         */
/* ========================================================================= */

void board_dac_cs_low(void)
{
    HAL_GPIO_WritePin(DAC_SYNC_GPIO_Port, DAC_SYNC_Pin, GPIO_PIN_RESET);
}

void board_dac_cs_high(void)
{
    HAL_GPIO_WritePin(DAC_SYNC_GPIO_Port, DAC_SYNC_Pin, GPIO_PIN_SET);
}

void board_dac_ldac_pulse(void)
{
    HAL_GPIO_WritePin(DAC_LDAC_GPIO_Port, DAC_LDAC_Pin, GPIO_PIN_RESET);
    /* Brief delay — LDAC minimum pulse width is 20ns, a few NOPs suffice */
    __asm volatile("nop\nnop\nnop\nnop");
    HAL_GPIO_WritePin(DAC_LDAC_GPIO_Port, DAC_LDAC_Pin, GPIO_PIN_SET);
}

void board_dac_clr_assert(void)
{
    HAL_GPIO_WritePin(DAC_CLR_GPIO_Port, DAC_CLR_Pin, GPIO_PIN_RESET);
}

void board_dac_clr_release(void)
{
    HAL_GPIO_WritePin(DAC_CLR_GPIO_Port, DAC_CLR_Pin, GPIO_PIN_SET);
}

/* ========================================================================= */
/*  ADC chip-select and DRDY                                                 */
/* ========================================================================= */

void board_adc_cs_low(void)
{
    HAL_GPIO_WritePin(ADC_CS_GPIO_Port, ADC_CS_Pin, GPIO_PIN_RESET);
}

void board_adc_cs_high(void)
{
    HAL_GPIO_WritePin(ADC_CS_GPIO_Port, ADC_CS_Pin, GPIO_PIN_SET);
}

uint8_t board_adc_drdy_read(void)
{
    return (uint8_t)HAL_GPIO_ReadPin(ADC_DRDY_GPIO_Port, ADC_DRDY_Pin);
}

void board_adc_sync_rst_assert(void)
{
    HAL_GPIO_WritePin(ADC_SYNC_RST_GPIO_Port, ADC_SYNC_RST_Pin, GPIO_PIN_RESET);
}

void board_adc_sync_rst_release(void)
{
    HAL_GPIO_WritePin(ADC_SYNC_RST_GPIO_Port, ADC_SYNC_RST_Pin, GPIO_PIN_SET);
}

/* ========================================================================= */
/*  Voltage / DAC code conversion                                            */
/* ========================================================================= */

float board_output_voltage_to_dac_pin_voltage(float voltage_v)
{
    /*
     * Output voltage: V_out = GAIN * V_dac + OFFSET
     * Solve for V_dac: V_dac = (V_out - OFFSET) / GAIN
     */
    float v_dac = (voltage_v - SUBTRACTOR_OFFSET_V) / SUBTRACTOR_GAIN;

    /* Clamp to DAC range [0, 2*VREF] */
    if (v_dac < 0.0f) {
        v_dac = 0.0f;
    }
    float v_max = DAC_VREF_V * 2.0f;
    if (v_dac > v_max) {
        v_dac = v_max;
    }

    return v_dac;
}

uint16_t board_voltage_to_dac_code(float voltage_v)
{
    /* DAC code: code = V_dac / (2 * VREF) * 65536 */
    float v_max = DAC_VREF_V * 2.0f;
    float v_dac = board_output_voltage_to_dac_pin_voltage(voltage_v);
    uint32_t code = (uint32_t)(v_dac / v_max * 65535.0f + 0.5f);
    if (code > 65535) {
        code = 65535;
    }
    return (uint16_t)code;
}

float board_dac_code_to_dac_pin_voltage(uint16_t code)
{
    return (float)code / 65535.0f * DAC_VREF_V * 2.0f;
}

float board_dac_code_to_voltage(uint16_t code)
{
    float v_dac = board_dac_code_to_dac_pin_voltage(code);
    return SUBTRACTOR_GAIN * v_dac + SUBTRACTOR_OFFSET_V;
}
