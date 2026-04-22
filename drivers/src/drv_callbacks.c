/**
 * HAL callback dispatch that overrides the HAL weak defaults.
 *
 * This file must stay outside cubemx/ because CubeMX regeneration would
 * otherwise clobber the user callback routing.
 */

#include "app_main_dpmzm.h"
#include "drv_ads131m02.h"
#include "drv_board.h"
#include "main.h"
#include "spi.h"
#include "usart.h"

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ADC_DRDY_Pin) {
        ads131m02_drdy_isr_handler();
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        app_dpmzm_pilot_spi_tx_cplt();
    }
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi2) {
        HAL_SPI_TxRxCpltCallback_ADC(hspi);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        app_dpmzm_pilot_spi_error();
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        board_uart_tx_cplt();
    }
}
