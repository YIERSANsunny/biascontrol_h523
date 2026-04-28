#ifndef DRV_BOARD_H
#define DRV_BOARD_H

#include <stdint.h>

/* ========================================================================= */
/*  Pin Definitions (from netlist: 偏压控制板_Netlist_PCB2_2026-03-21.tel)   */
/* ========================================================================= */

/* --- SPI1: DAC8568 (U7) --- */
#define DAC_SPI_INSTANCE        SPI1
#define DAC_CLK_PORT            GPIOA
#define DAC_CLK_PIN             GPIO_PIN_5   /* PA5 */
#define DAC_DIN_PORT            GPIOA
#define DAC_DIN_PIN             GPIO_PIN_7   /* PA7 */
#define DAC_SYNC_PORT           GPIOB
#define DAC_SYNC_PIN            GPIO_PIN_1   /* PB1 - chip select (active low) */
#define DAC_LDAC_PORT           GPIOB
#define DAC_LDAC_PIN            GPIO_PIN_2   /* PB2 (U1.20, RN1.4↔RN1.5) */
#define DAC_CLR_PORT            GPIOB
#define DAC_CLR_PIN             GPIO_PIN_0   /* PB0 (U1.18, RN2.1↔RN2.8) */

/* --- SPI2: ADS131M02 (U8) --- */
#define ADC_SPI_INSTANCE        SPI2
#define ADC_SCLK_PORT           GPIOB
#define ADC_SCLK_PIN            GPIO_PIN_13  /* PB13 */
#define ADC_MOSI_PORT           GPIOB
#define ADC_MOSI_PIN            GPIO_PIN_15  /* PB15 */
#define ADC_MISO_PORT           GPIOB
#define ADC_MISO_PIN            GPIO_PIN_14  /* PB14 */
#define ADC_CLKIN_PORT          GPIOA
#define ADC_CLKIN_PIN           GPIO_PIN_8   /* PA8 (U1.29, RN2.2↔RN2.7) - MCO1 output 8.192MHz */
#define ADC_CS_PORT             GPIOB
#define ADC_CS_PIN              GPIO_PIN_12  /* PB12 (U1.25, RN3.3↔RN3.6) */
#define ADC_DRDY_PORT           GPIOA
#define ADC_DRDY_PIN            GPIO_PIN_11  /* PA11 (U1.32, RN3.2↔RN3.7) - data ready (active low) */
#define ADC_DRDY_EXTI_IRQn      EXTI11_IRQn
#define ADC_SYNC_RST_PORT       GPIOA
#define ADC_SYNC_RST_PIN        GPIO_PIN_12  /* PA12 (U1.33, RN3.4↔RN3.5) - /SYNC/RESET (active low) */

/* --- USART1: Debug/Tuning Interface --- */
#define DEBUG_USART_INSTANCE    USART1
#define DEBUG_TX_PORT           GPIOA
#define DEBUG_TX_PIN            GPIO_PIN_9   /* PA9 */
#define DEBUG_RX_PORT           GPIOA
#define DEBUG_RX_PIN            GPIO_PIN_10  /* PA10 */

/* --- LED --- */
#define LED_PORT                GPIOC
#define LED_PIN                 GPIO_PIN_13  /* PC13 (U1.2, via R2 to LED1) */

/* --- Comparator output (LM211, U6) --- */
#define COMP_OUT_PORT           GPIOA
#define COMP_OUT_PIN            GPIO_PIN_0   /* PA0 */

/* --- DAC output channel mapping --- */
/* DAC8568 channels VOA-VOH map to output connectors VA-VH via subtractor */
/* The subtractor converts DAC 0~5V to output -10V~+10V                   */
/* Gain = 4x, Offset = -10V. Output = 4 * V_DAC - 10V                    */
#define DAC_CH_VA               0  /* VOA -> VA (U11.8) */
#define DAC_CH_VB               1  /* VOB -> VB (U11.7) */
#define DAC_CH_VC               2  /* VOC -> VC (U11.6) */
#define DAC_CH_VD               3  /* VOD -> VD (U11.5) */
#define DAC_CH_VE               4  /* VOE -> VE (U11.4) */
#define DAC_CH_VF               5  /* VOF -> VF (U11.3) */
#define DAC_CH_VG               6  /* VOG -> VG (U11.2) */
#define DAC_CH_VH               7  /* VOH -> VH (U11.1) */

/* --- Subtractor circuit parameters --- */
/* V_out = GAIN * V_dac + OFFSET                                          */
/* With VREF=2.5V, R_in=12k, R_f=120k, R_ref=30k, R_gnd=3k:             */
/* V_out = (120k/12k) * V_dac - (120k/30k) * VREF = 10*V_dac - 10V      */
/* Actually: V_out ranges from -10V (V_dac=0) to +10V (V_dac=5V/2=2.5V)  */
/* Need to verify exact gain from resistor network on board               */
#define SUBTRACTOR_GAIN         4.0f    /* To be verified on hardware */
#define SUBTRACTOR_OFFSET_V     (-10.0f)

/* --- DAC8568 parameters --- */
#define DAC_VREF_V              2.5f    /* Internal reference voltage */
#define DAC_RESOLUTION          65536   /* 16-bit */
#define DAC_LSB_V               (DAC_VREF_V * 2.0f / DAC_RESOLUTION)

/* --- ADC ADS131M02 parameters --- */
#define ADC_CHANNELS            2
#define ADC_RESOLUTION_BITS     24
#define ADC_SAMPLE_RATE_HZ      64000  /* OSR=128, HR mode, 8.192 MHz CLKIN */

/* ========================================================================= */
/*  Board-level functions                                                    */
/* ========================================================================= */

/**
 * Initialize all board peripherals: clocks, GPIO, SPI, USART, timers, EXTI.
 * Must be called once at startup before any other driver function.
 */
void board_init(void);

/** LED control */
void board_led_on(void);
void board_led_off(void);
void board_led_toggle(void);

/** Simple blocking delay (ms) using SysTick */
void board_delay_ms(uint32_t ms);

/** Return millisecond tick counter (wraps at 2^32 ms ≈ 49 days) */
uint32_t board_get_tick_ms(void);

/** DAC chip-select control (active low) */
void board_dac_cs_low(void);
void board_dac_cs_high(void);

/** DAC LDAC pulse (loads all DAC registers simultaneously) */
void board_dac_ldac_pulse(void);

/** DAC CLR pin control */
void board_dac_clr_assert(void);
void board_dac_clr_release(void);

/** ADC chip-select control (active low) */
void board_adc_cs_low(void);
void board_adc_cs_high(void);

/** Check ADC DRDY pin state (returns 0 when data ready) */
uint8_t board_adc_drdy_read(void);

/** ADC /SYNC/RESET pin control (active low) */
void board_adc_sync_rst_assert(void);
void board_adc_sync_rst_release(void);

/**
 * Convert output voltage (-10V to +10V) to DAC code (0-65535).
 * Accounts for subtractor gain and offset.
 */
uint16_t board_voltage_to_dac_code(float voltage_v);

/**
 * Convert desired board output voltage to the raw DAC pin voltage.
 *
 * This is useful for debug/status output because the DAC pin itself sits at
 * a positive voltage (for example ~2.5 V when the modeled board output is
 * 0 V through the subtractor stage).
 */
float board_output_voltage_to_dac_pin_voltage(float voltage_v);

/**
 * Convert DAC code to output voltage.
 */
float board_dac_code_to_voltage(uint16_t code);

/**
 * Convert DAC code to the raw DAC pin voltage.
 */
float board_dac_code_to_dac_pin_voltage(uint16_t code);

/**
 * Called from HAL_UART_TxCpltCallback (ISR context) when USART1 DMA TX
 * completes. Clears the internal tx-busy flag so the next _write() can run.
 */
void board_uart_tx_cplt(void);

#endif /* DRV_BOARD_H */
