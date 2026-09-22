#ifndef BSP_UART2_H
#define BSP_UART2_H

#include <stdint.h>
#include "stm32l4xx_hal.h"

#define BSP_UART2_TX_MAX_LENGTH 16U

typedef struct {
  volatile uint32_t rx_drop_count;
  volatile uint32_t tx_drop_count;
  volatile uint32_t error_count;
  volatile uint32_t recover_count;
  volatile uint32_t last_error_code;
} BSP_Uart2Diagnostics_t;

extern BSP_Uart2Diagnostics_t bsp_uart2_diagnostics;

void BSP_Uart2_Init(void);
void BSP_Uart2_Task(void);
uint8_t BSP_Uart2_Send(const uint8_t *data, uint16_t length);
uint8_t BSP_Uart2_ReadByte(uint8_t *data);
void BSP_Uart2_OnTxCplt(UART_HandleTypeDef *huart);
void BSP_Uart2_OnRxCplt(UART_HandleTypeDef *huart);
void BSP_Uart2_OnError(UART_HandleTypeDef *huart);

#endif
