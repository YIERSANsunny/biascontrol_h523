/**
 * HAL callback dispatch — overrides HAL weak defaults.
 *
 * HAL declares HAL_GPIO_EXTI_Falling_Callback and HAL_SPI_TxRxCpltCallback
 * as weak symbols.  Defining them here (in a tracked source file) overrides
 * the no-op stubs and routes each event to the appropriate driver.
 *
 * ADS131M02 (SPI2) uses DMA full-duplex — needs TX/RX-complete callback.
 * DAC8568 (SPI1) uses DMA TX — needs TX-complete callback (spec-07).
 *
 * This file must NOT be placed inside cubemx/ (gitignored).
 */

#include "app_main_dpmzm.h"
#include "drv_ads131m02.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include "main.h"   /* ADC_DRDY_Pin macro from CubeMX */
#include "spi.h"    /* hspi1, hspi2 extern declaration */
#include "usart.h"  /* huart1 extern declaration */

/* -------------------------------------------------------------------------
 * EXTI11 (ADC DRDY falling edge) → ADS131M02 DMA read trigger
 * ------------------------------------------------------------------------- */
void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ADC_DRDY_Pin) {
        ads131m02_drdy_isr_handler();
    }
}

/* -------------------------------------------------------------------------
 * SPI2 TX+RX complete (ADS131M02 frame received) → parse and forward
 * ------------------------------------------------------------------------- */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi2) {
        HAL_SPI_TxRxCpltCallback_ADC(hspi);
    }
}

/* -------------------------------------------------------------------------
 * SPI1 TX DMA complete (DAC8568) → finish CS/LDAC sequence (spec-07)
 * ------------------------------------------------------------------------- */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        if (app_dpmzm_pilot_output_active()) {
            app_dpmzm_pilot_spi_tx_cplt();
        } else {
            dac8568_dma_tx_cplt();
        }
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1 && app_dpmzm_pilot_output_active()) {
        app_dpmzm_pilot_spi_error();
    }
}

/* -------------------------------------------------------------------------
 * USART1 TX DMA complete → clear tx-busy flag in app_uart
 * ------------------------------------------------------------------------- */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        board_uart_tx_cplt();
    }
}
