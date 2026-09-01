#include "bsp_uart.h"
#include "usart.h"

#define BSP_UART_TX_QUEUE_DEPTH 4U
#define BSP_UART_RX_BUFFER_SIZE 128U

static uint8_t uart_tx_queue[BSP_UART_TX_QUEUE_DEPTH][BSP_UART_TX_MAX_LENGTH];
static uint16_t uart_tx_length[BSP_UART_TX_QUEUE_DEPTH];
static volatile uint8_t uart_tx_head;
static volatile uint8_t uart_tx_tail;
static volatile uint8_t uart_tx_busy;
static uint8_t uart_rx_byte;
static uint8_t uart_rx_buffer[BSP_UART_RX_BUFFER_SIZE];
static volatile uint8_t uart_rx_head;
static volatile uint8_t uart_rx_tail;

static void BSP_Uart_StartNextTx(void)
{
  if ((uart_tx_busy != 0U) || (uart_tx_tail == uart_tx_head))
  {
    return;
  }

  uart_tx_busy = 1U;
  if (HAL_UART_Transmit_IT(&hlpuart1, uart_tx_queue[uart_tx_tail],
                           uart_tx_length[uart_tx_tail]) != HAL_OK)
  {
    uart_tx_busy = 0U;
  }
}

void BSP_Uart_Init(void)
{
  HAL_NVIC_SetPriority(LPUART1_IRQn, 14U, 0U);
  HAL_NVIC_EnableIRQ(LPUART1_IRQn);
  uart_tx_head = 0U;
  uart_tx_tail = 0U;
  uart_tx_busy = 0U;
  uart_rx_head = 0U;
  uart_rx_tail = 0U;
  (void)HAL_UART_Receive_IT(&hlpuart1, &uart_rx_byte, 1U);
}

void BSP_Uart_Task(void)
{
  BSP_Uart_StartNextTx();
}

uint8_t BSP_Uart_Send(const uint8_t *data, uint16_t length)
{
  uint8_t next_head;
  uint16_t index;

  if ((data == NULL) || (length == 0U) ||
      (length > BSP_UART_TX_MAX_LENGTH))
  {
    return 0U;
  }

  next_head = (uint8_t)((uart_tx_head + 1U) % BSP_UART_TX_QUEUE_DEPTH);
  if (next_head == uart_tx_tail)
  {
    return 0U;
  }

  for (index = 0U; index < length; index++)
  {
    uart_tx_queue[uart_tx_head][index] = data[index];
  }
  uart_tx_length[uart_tx_head] = length;
  uart_tx_head = next_head;
  BSP_Uart_StartNextTx();
  return 1U;
}

uint8_t BSP_Uart_ReadByte(uint8_t *data)
{
  if ((data == NULL) || (uart_rx_tail == uart_rx_head))
  {
    return 0U;
  }

  *data = uart_rx_buffer[uart_rx_tail];
  uart_rx_tail = (uint8_t)((uart_rx_tail + 1U) % BSP_UART_RX_BUFFER_SIZE);
  return 1U;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if ((huart != NULL) && (huart->Instance == LPUART1))
  {
    uart_tx_tail = (uint8_t)((uart_tx_tail + 1U) % BSP_UART_TX_QUEUE_DEPTH);
    uart_tx_busy = 0U;
    BSP_Uart_StartNextTx();
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  uint8_t next_head;

  if ((huart == NULL) || (huart->Instance != LPUART1))
  {
    return;
  }

  next_head = (uint8_t)((uart_rx_head + 1U) % BSP_UART_RX_BUFFER_SIZE);
  if (next_head != uart_rx_tail)
  {
    uart_rx_buffer[uart_rx_head] = uart_rx_byte;
    uart_rx_head = next_head;
  }
  (void)HAL_UART_Receive_IT(&hlpuart1, &uart_rx_byte, 1U);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if ((huart != NULL) && (huart->Instance == LPUART1))
  {
    (void)HAL_UART_Receive_IT(&hlpuart1, &uart_rx_byte, 1U);
  }
}
