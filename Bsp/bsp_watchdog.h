#ifndef BSP_WATCHDOG_H
#define BSP_WATCHDOG_H

#include <stdint.h>

uint8_t BSP_Watchdog_Init(void);
uint8_t BSP_Watchdog_WasReset(void);
void BSP_Watchdog_Feed(void);

#endif /* BSP_WATCHDOG_H */
