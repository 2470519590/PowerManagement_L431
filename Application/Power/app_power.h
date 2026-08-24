#ifndef APP_POWER_H
#define APP_POWER_H

#include <stdint.h>
#include "bsp_adc.h"

typedef enum
{
  POWER_SWITCH_ON = 0,
  POWER_SWITCH_OFF = 1
} PowerSwitchState_t;

typedef struct
{
  uint16_t adc_ammo_raw;
  uint32_t ammo_current_ma;
  uint32_t ammo_power_w;
  uint32_t ammo_power_avg_w;
  uint32_t ammo_power_peak_w;
  uint8_t power_limit_latched;
  BSP_AdcStatus_t adc_status;
  uint32_t adc_attempt_count;
  uint32_t adc_success_count;
  uint32_t adc_last_sample_tick;
  PowerSwitchState_t switch_ammo;
} PowerMonitorState_t;

extern volatile PowerMonitorState_t power_monitor;

void APP_Power_Init(void);
void APP_Power_ReportTask(void);
void APP_Power_Task(void);
void APP_Power_OverLimitCallback(void);

#endif /* APP_POWER_H */
