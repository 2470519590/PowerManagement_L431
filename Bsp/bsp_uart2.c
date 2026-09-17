#include "bsp_uart2.h"
#include "usart.h"

#define BSP_UART2_TX_QUEUE_DEPTH 8U
#define BSP_UART2_RX_BUFFER_SIZE 128U

static uint8_t uart2_tx_queue[BSP_UART2_TX_QUEUE_DEPTH][BSP_UART2_TX_MAX_LENGTH];
static uint16_t uart2_tx_length[BSP_UART2_TX_QUEUE_DEPTH];
static volatile uint8_t uart2_tx_head;
static volatile uint8_t uart2_tx_tail;
static volatile uint8_t uart2_tx_busy;
static uint8_t uart2_rx_byte;
static uint8_t uart2_rx_buffer[BSP_UART2_RX_BUFFER_SIZE];
static volatile uint8_t uart2_rx_head;
static volatile uint8_t uart2_rx_tail;

static void BSP_Uart2_StartNextTx(void)
{
  if ((uart2_tx_busy != 0U) || (uart2_tx_tail == uart2_tx_head)) return;
  uart2_tx_busy = 1U;
  if (HAL_UART_Transmit_IT(&huart2, uart2_tx_queue[uart2_tx_tail],
                           uart2_tx_length[uart2_tx_tail]) != HAL_OK)
  {
    uart2_tx_busy = 0U;
  }
}

void BSP_Uart2_Init(void)
{
  uart2_tx_head = 0U; uart2_tx_tail = 0U; uart2_tx_busy = 0U;
  uart2_rx_head = 0U; uart2_rx_tail = 0U;
  HAL_NVIC_SetPriority(USART2_IRQn, 13U, 0U);
  HAL_NVIC_EnableIRQ(USART2_IRQn);
  (void)HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1U);
}

void BSP_Uart2_Task(void) { BSP_Uart2_StartNextTx(); }

uint8_t BSP_Uart2_Send(const uint8_t *data, uint16_t length)
{
  uint8_t next; uint16_t i;
  if (data == NULL || length == 0U || length > BSP_UART2_TX_MAX_LENGTH) return 0U;
  next = (uint8_t)((uart2_tx_head + 1U) % BSP_UART2_TX_QUEUE_DEPTH);
  if (next == uart2_tx_tail) return 0U;
  for (i = 0U; i < length; i++) uart2_tx_queue[uart2_tx_head][i] = data[i];
  uart2_tx_length[uart2_tx_head] = length;
  uart2_tx_head = next;
  BSP_Uart2_StartNextTx();
  return 1U;
}

uint8_t BSP_Uart2_ReadByte(uint8_t *data)
{
  if (data == NULL || uart2_rx_tail == uart2_rx_head) return 0U;
  *data = uart2_rx_buffer[uart2_rx_tail];
  uart2_rx_tail = (uint8_t)((uart2_rx_tail + 1U) % BSP_UART2_RX_BUFFER_SIZE);
  return 1U;
}

void BSP_Uart2_OnTxCplt(UART_HandleTypeDef *huart)
{
  if (huart == NULL || huart->Instance != USART2) return;
  uart2_tx_tail = (uint8_t)((uart2_tx_tail + 1U) % BSP_UART2_TX_QUEUE_DEPTH);
  uart2_tx_busy = 0U;
  BSP_Uart2_StartNextTx();
}

void BSP_Uart2_OnRxCplt(UART_HandleTypeDef *huart)
{
  uint8_t next;
  if (huart == NULL || huart->Instance != USART2) return;
  next = (uint8_t)((uart2_rx_head + 1U) % BSP_UART2_RX_BUFFER_SIZE);
  if (next != uart2_rx_tail) { uart2_rx_buffer[uart2_rx_head] = uart2_rx_byte; uart2_rx_head = next; }
  (void)HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1U);
}

void BSP_Uart2_OnError(UART_HandleTypeDef *huart)
{
  if (huart != NULL && huart->Instance == USART2) (void)HAL_UART_Receive_IT(&huart2, &uart2_rx_byte, 1U);
}
