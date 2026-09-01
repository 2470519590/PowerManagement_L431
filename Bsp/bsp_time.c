#include "bsp_time.h"
#include <stddef.h>

static BSP_Time1msCallback_t time_callbacks[BSP_TIME_MAX_1MS_CALLBACKS];

uint8_t BSP_Time_Register1msCallback(BSP_Time1msCallback_t callback)
{
  uint32_t index;

  if (callback == NULL)
  {
    return 0U;
  }

  for (index = 0U; index < BSP_TIME_MAX_1MS_CALLBACKS; index++)
  {
    if (time_callbacks[index] == callback)
    {
      return 1U;
    }
    if (time_callbacks[index] == NULL)
    {
      time_callbacks[index] = callback;
      return 1U;
    }
  }

  return 0U;
}

void HAL_SYSTICK_Callback(void)
{
  uint32_t index;

  for (index = 0U; index < BSP_TIME_MAX_1MS_CALLBACKS; index++)
  {
    if (time_callbacks[index] != NULL)
    {
      time_callbacks[index]();
    }
  }
}
