#ifndef APP_POWER_H
#define APP_POWER_H

#include <stdint.h>

typedef enum
{
  POWER_SWITCH_ON = 0,
  POWER_SWITCH_OFF = 1
} PowerSwitchState_t;

typedef enum
{
  POWER_FAULT_NONE = 0,
  POWER_FAULT_POWER_CUT,
  POWER_FAULT_OVERCURRENT,
  POWER_FAULT_ADC,
  POWER_FAULT_WATCHDOG
} PowerFault_t;

typedef struct
{
  uint16_t adc_chassis_raw;
  uint32_t chassis_current_ma;
  uint32_t chassis_current_avg_ma;
  uint32_t chassis_power_w;
  uint32_t chassis_power_avg_w;
  uint32_t chassis_power_peak_w;
  uint16_t power_buffer_deci_j;
  uint16_t power_limit_w;
  uint32_t current_limit_ma;
  uint8_t power_limit_exceeded;
  uint8_t measurement_saturation_risk;
  uint8_t power_cut_active;
  uint8_t current_overload_active;
  uint32_t power_cut_count;
  uint32_t current_overload_count;
  uint32_t adc_start_count;
  uint32_t adc_success_count;
  uint32_t adc_consecutive_failure_count;
  uint8_t adc_fault_active;
  uint8_t fire_prohibited;
  PowerFault_t last_fault;
  PowerSwitchState_t switch_chassis;
  PowerSwitchState_t switch_reserved;
} PowerMonitorState_t;

typedef struct
{
  uint32_t chassis_power_avg_w;
  uint32_t chassis_power_peak_w;
  uint32_t chassis_current_avg_ma;
  uint8_t power_limit_exceeded;
  uint8_t measurement_saturation_risk;
  uint16_t power_buffer_deci_j;
  uint16_t power_limit_w;
  uint32_t current_limit_ma;
  uint8_t power_cut_active;
  uint8_t current_overload_active;
  uint8_t adc_fault_active;
  uint8_t fire_prohibited;
  PowerSwitchState_t switch_chassis;
  PowerSwitchState_t switch_reserved;
} PowerReportSnapshot_t;

typedef struct
{
  uint16_t adc_chassis_raw;
  uint32_t chassis_current_ma;
  uint32_t chassis_power_w;
  uint32_t adc_start_count;
  uint32_t adc_success_count;
  uint32_t adc_consecutive_failure_count;
  uint8_t measurement_saturation_risk;
} PowerRawSnapshot_t;

typedef struct
{
  uint32_t power_cut_count;
  uint32_t current_overload_count;
  uint8_t power_cut_active;
  uint8_t current_overload_active;
  uint8_t adc_fault_active;
  PowerFault_t last_fault;
} PowerFaultSnapshot_t;

extern volatile PowerMonitorState_t power_monitor;

void APP_Power_Init(uint8_t watchdog_reset_detected);
uint8_t APP_Power_GetLatestReportSnapshot(PowerReportSnapshot_t *snapshot,
                                           uint32_t *sequence);
void APP_Power_GetRawSnapshot(PowerRawSnapshot_t *snapshot);
void APP_Power_GetFaultSnapshot(PowerFaultSnapshot_t *snapshot);
uint8_t APP_Power_GetFireEvent(uint8_t *prohibited, uint32_t *sequence);
uint8_t APP_Power_SetApplicationPowerLimit(uint16_t power_limit_w);
uint8_t APP_Power_SetApplicationBufferEnergy(uint16_t buffer_energy_deci_j);
uint8_t APP_Power_SetSafetyCurrentLimit(uint32_t current_limit_ma);
uint8_t APP_Power_ForceChassisState(PowerSwitchState_t state);
uint8_t APP_Power_ForceReservedState(PowerSwitchState_t state);
uint8_t APP_Power_RequestChassisOn(void);
uint8_t APP_Power_RequestReservedOn(void);
void APP_Power_OverLimitCallback(void);

#endif /* APP_POWER_H */
