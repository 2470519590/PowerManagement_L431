#ifndef APP_MATCH_H
#define APP_MATCH_H

#include <stdint.h>

void APP_Match_Init(void);
void APP_Match_Task(void);
uint8_t APP_Match_GetShootPermission(uint8_t *enabled, uint32_t *sequence);

#endif
