#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>

typedef struct
{
  uint16_t standard_id;
  uint8_t dlc;
  uint8_t data[8];
} BSP_CanFrame_t;

typedef struct
{
  volatile uint32_t rx_count;
  volatile uint32_t rx_drop_count;
  volatile uint32_t tx_count;
  volatile uint32_t tx_error_count;
  volatile uint32_t error_count;
  volatile uint32_t last_error_code;
  volatile uint32_t bus_off_count;
  volatile uint32_t recovery_count;
  volatile uint8_t ready;
} BSP_CanDiagnostics_t;

extern BSP_CanDiagnostics_t bsp_can_diagnostics;

/* Direct debugger variables. Counts are updated by the CAN ISR or CAN task. */
extern volatile uint32_t CAN_RX_COUNT;
extern volatile uint32_t CAN_RX_DROP_COUNT;
extern volatile uint32_t CAN_TX_COUNT;
extern volatile uint32_t CAN_TX_ERROR_COUNT;
extern volatile uint32_t CAN_ERROR_COUNT;
extern volatile uint32_t CAN_LAST_ERROR_CODE;
extern volatile uint32_t CAN_BUS_OFF_COUNT;
extern volatile uint32_t CAN_RECOVERY_COUNT;
extern volatile uint8_t CAN_READY;

uint8_t BSP_Can_Init(void);
void BSP_Can_Task(void);
uint8_t BSP_Can_Read(BSP_CanFrame_t *frame);
uint8_t BSP_Can_Send(uint16_t standard_id, const uint8_t *data, uint8_t dlc);

#endif /* BSP_CAN_H */
