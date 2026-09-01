#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>

#define BSP_UART_TX_MAX_LENGTH 128U

void BSP_Uart_Init(void);
void BSP_Uart_Task(void);
uint8_t BSP_Uart_Send(const uint8_t *data, uint16_t length);
uint8_t BSP_Uart_ReadByte(uint8_t *data);

#endif /* BSP_UART_H */
