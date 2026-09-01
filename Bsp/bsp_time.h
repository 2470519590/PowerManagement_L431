#ifndef BSP_TIME_H
#define BSP_TIME_H

#include <stdint.h>

#define BSP_TIME_MAX_1MS_CALLBACKS 4U

typedef void (*BSP_Time1msCallback_t)(void);

uint8_t BSP_Time_Register1msCallback(BSP_Time1msCallback_t callback);

#endif /* BSP_TIME_H */
