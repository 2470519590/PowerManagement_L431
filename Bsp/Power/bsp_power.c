#include "bsp_power.h"

void BSP_Power_Init(void)
{
  /* AMMO is the only enabled power path on this PCB. */
  HAL_GPIO_WritePin(INT_CHASSIS_GPIO_Port, INT_CHASSIS_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(INT_GIMBAL_GPIO_Port, INT_GIMBAL_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(INT_MINI_PC_GPIO_Port, INT_MINI_PC_Pin, GPIO_PIN_RESET);
}

void BSP_Power_AmmoOn(void)
{
  /* PCB control polarity is active-low. */
  HAL_GPIO_WritePin(INT_AMMO_GPIO_Port, INT_AMMO_Pin, GPIO_PIN_RESET);
}

void BSP_Power_AmmoOff(void)
{
  /* PCB control polarity is active-low. */
  HAL_GPIO_WritePin(INT_AMMO_GPIO_Port, INT_AMMO_Pin, GPIO_PIN_SET);
}
