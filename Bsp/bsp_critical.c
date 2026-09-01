#include "bsp_critical.h"
#include "main.h"

uint32_t BSP_Critical_Enter(void)
{
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  return primask;
}

void BSP_Critical_Exit(uint32_t primask)
{
  if (primask == 0U)
  {
    __enable_irq();
  }
}
