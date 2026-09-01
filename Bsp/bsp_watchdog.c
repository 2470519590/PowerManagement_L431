#include "bsp_watchdog.h"
#include "main.h"

#define BSP_WATCHDOG_PRESCALER_DIV32 3U
#define BSP_WATCHDOG_RELOAD_VALUE    999U
#define BSP_WATCHDOG_KEY_ENABLE      0xCCCCU
#define BSP_WATCHDOG_KEY_WRITE       0x5555U
#define BSP_WATCHDOG_KEY_RELOAD      0xAAAAU

static uint8_t watchdog_reset_detected;

uint8_t BSP_Watchdog_Init(void)
{
  watchdog_reset_detected =
      (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET) ? 1U : 0U;
  __HAL_RCC_CLEAR_RESET_FLAGS();

  __HAL_RCC_LSI_ENABLE();
  while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == RESET)
  {
  }

  IWDG->KR = BSP_WATCHDOG_KEY_ENABLE;
  IWDG->KR = BSP_WATCHDOG_KEY_WRITE;
  IWDG->PR = BSP_WATCHDOG_PRESCALER_DIV32;
  IWDG->RLR = BSP_WATCHDOG_RELOAD_VALUE;
  while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) != 0U)
  {
  }
  IWDG->KR = BSP_WATCHDOG_KEY_RELOAD;
  __HAL_DBGMCU_FREEZE_IWDG();
  return 1U;
}

uint8_t BSP_Watchdog_WasReset(void)
{
  return watchdog_reset_detected;
}

void BSP_Watchdog_Feed(void)
{
  IWDG->KR = BSP_WATCHDOG_KEY_RELOAD;
}
