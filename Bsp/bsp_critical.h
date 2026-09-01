#ifndef BSP_CRITICAL_H
#define BSP_CRITICAL_H

#include <stdint.h>

uint32_t BSP_Critical_Enter(void);
void BSP_Critical_Exit(uint32_t primask);

#endif /* BSP_CRITICAL_H */
