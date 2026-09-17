#include "app_power.h"
#include "bsp_adc.h"
#include "bsp_power.h"
#include "bsp_time.h"
#include "bsp_critical.h"
#include "app_power_config.h"
#include "bsp_ina180.h"
#include <stddef.h>

volatile PowerMonitorState_t power_monitor =
{
  .switch_chassis = POWER_SWITCH_ON
};

static uint32_t power_sample_elapsed_ms;
static uint32_t power_settlement_elapsed_ms;
static uint64_t power_window_sum_w;
static uint64_t power_window_sum_ma;
static uint32_t power_window_peak_w;
static uint8_t power_window_saturation_risk;
static uint32_t power_window_sample_count;
static uint32_t power_cut_remaining_ms;
static uint32_t power_overcurrent_elapsed_ms;
static volatile uint8_t power_report_valid;
static volatile uint8_t power_settlement_due;
static uint8_t power_sampling_enabled;
static volatile uint32_t power_report_sequence;
static volatile PowerReportSnapshot_t power_report_snapshot;
static volatile uint8_t power_fire_prohibited;
static volatile uint32_t power_fire_event_sequence;

static void APP_Power_OnChassisSample(uint16_t adc_raw);
static void APP_Power_SettlePowerLimit(void);
static void APP_Power_Tick1ms(void);

static void APP_Power_SetFirePermission(uint8_t prohibited)
{
  if (power_fire_prohibited != prohibited)
  {
    power_fire_prohibited = prohibited;
    power_fire_event_sequence++;
  }
}

static void APP_Power_SetChassisState(PowerSwitchState_t state)
{
  if (state == POWER_SWITCH_ON)
  {
    BSP_Power_AmmoOn();
  }
  else
  {
    BSP_Power_AmmoOff();
  }
  power_monitor.switch_chassis = state;
}

static uint8_t APP_Power_CanTurnChassisOn(void)
{
  return ((power_sampling_enabled != 0U) &&
          (power_monitor.power_cut_active == 0U) &&
          (power_monitor.current_overload_active == 0U) &&
          (power_monitor.adc_fault_active == 0U)) ? 1U : 0U;
}

void APP_Power_Init(uint8_t watchdog_reset_detected)
{
  BSP_Power_Init();
  APP_Power_SetChassisState(POWER_SWITCH_OFF);
  power_monitor.adc_chassis_raw = 0U;
  power_monitor.chassis_current_ma = 0U;
  power_monitor.chassis_current_avg_ma = 0U;
  power_monitor.chassis_power_w = 0U;
  power_monitor.chassis_power_avg_w = 0U;
  power_monitor.chassis_power_peak_w = 0U;
  power_monitor.power_buffer_deci_j = APP_POWER_BUFFER_MAX_DECI_J;
  power_monitor.power_limit_w = APP_POWER_LIMIT_W;
  power_monitor.current_limit_ma = APP_POWER_CURRENT_LIMIT_MA;
  power_monitor.power_limit_exceeded = 0U;
  power_monitor.measurement_saturation_risk = 0U;
  power_monitor.power_cut_active = 0U;
  power_monitor.current_overload_active = 0U;
  power_monitor.power_cut_count = 0U;
  power_monitor.current_overload_count = 0U;
  power_monitor.adc_start_count = 0U;
  power_monitor.adc_success_count = 0U;
  power_monitor.adc_consecutive_failure_count = 0U;
  power_monitor.adc_fault_active = 0U;
  power_monitor.fire_prohibited = 0U;
  power_monitor.last_fault = POWER_FAULT_NONE;
  power_monitor.switch_reserved = POWER_SWITCH_OFF;
  power_sample_elapsed_ms = 0U;
  power_settlement_elapsed_ms = 0U;
  power_window_sum_w = 0U;
  power_window_sum_ma = 0U;
  power_window_peak_w = 0U;
  power_window_saturation_risk = 0U;
  power_window_sample_count = 0U;
  power_cut_remaining_ms = 0U;
  power_overcurrent_elapsed_ms = 0U;
  power_report_valid = 0U;
  power_settlement_due = 0U;
  power_report_sequence = 0U;
  power_report_snapshot = (PowerReportSnapshot_t){0};
  power_fire_prohibited = 0U;
  power_fire_event_sequence = 0U;
  power_sampling_enabled = 0U;

  BSP_Adc_RegisterAmmoCallback(APP_Power_OnChassisSample);
  if (BSP_Time_Register1msCallback(APP_Power_Tick1ms) == 0U)
  {
    power_monitor.adc_fault_active = 1U;
    power_monitor.last_fault = POWER_FAULT_ADC;
    power_monitor.fire_prohibited = 1U;
    APP_Power_SetFirePermission(1U);
    APP_Power_SetChassisState(POWER_SWITCH_OFF);
    return;
  }
  if (BSP_Adc_CalibrateAmmo() == 0U)
  {
    power_monitor.adc_fault_active = 1U;
    power_monitor.last_fault = POWER_FAULT_ADC;
    power_monitor.fire_prohibited = 1U;
    APP_Power_SetFirePermission(1U);
    APP_Power_SetChassisState(POWER_SWITCH_OFF);
    return;
  }
  power_sampling_enabled = 1U;
  if (watchdog_reset_detected == 0U)
  {
    APP_Power_SetChassisState(POWER_SWITCH_ON);
  }
  else
  {
    power_monitor.last_fault = POWER_FAULT_WATCHDOG;
    power_monitor.fire_prohibited = 1U;
    APP_Power_SetFirePermission(1U);
  }
  if (watchdog_reset_detected == 0U)
  {
    APP_Power_SetFirePermission(0U);
  }
}

static void APP_Power_Tick1ms(void)
{
  if (power_cut_remaining_ms > 0U)
  {
    power_cut_remaining_ms--;
    if (power_cut_remaining_ms == 0U)
    {
      power_monitor.power_cut_active = 0U;
      if (APP_Power_CanTurnChassisOn() != 0U)
      {
        APP_Power_SetChassisState(POWER_SWITCH_ON);
        power_monitor.fire_prohibited = 0U;
        APP_Power_SetFirePermission(0U);
      }
    }
  }

  if (power_sampling_enabled == 0U)
  {
    return;
  }

  power_sample_elapsed_ms++;
  power_settlement_elapsed_ms++;

  if (power_settlement_elapsed_ms >= APP_POWER_SETTLEMENT_PERIOD_MS)
  {
    power_settlement_elapsed_ms = 0U;
    power_settlement_due = 1U;
  }

  if (power_sample_elapsed_ms >= APP_POWER_ADC_SAMPLE_PERIOD_MS)
  {
    BSP_AdcStatus_t adc_status;

    power_monitor.adc_start_count++;
    adc_status = BSP_Adc_StartAmmo();
    if (adc_status == BSP_ADC_STATUS_OK)
    {
      power_monitor.adc_consecutive_failure_count = 0U;
    }
    else
    {
      power_monitor.adc_consecutive_failure_count++;
      if (power_monitor.adc_consecutive_failure_count >=
          APP_POWER_ADC_MAX_CONSECUTIVE_FAILURES)
      {
        power_monitor.adc_fault_active = 1U;
        power_monitor.last_fault = POWER_FAULT_ADC;
        power_monitor.fire_prohibited = 1U;
        APP_Power_SetFirePermission(1U);
        power_sampling_enabled = 0U;
        APP_Power_SetChassisState(POWER_SWITCH_OFF);
      }
    }
    power_sample_elapsed_ms = 0U;
  }
}

uint8_t APP_Power_GetLatestReportSnapshot(PowerReportSnapshot_t *snapshot,
                                          uint32_t *sequence)
{
  uint32_t sequence_begin;
  uint32_t sequence_end = 0U;

  if ((snapshot == NULL) || (sequence == NULL) || (power_report_valid == 0U))
  {
    return 0U;
  }

  do
  {
    sequence_begin = power_report_sequence;
    if ((sequence_begin & 1U) != 0U)
    {
      continue;
    }
    *snapshot = power_report_snapshot;
    sequence_end = power_report_sequence;
  } while ((sequence_begin != sequence_end) || ((sequence_end & 1U) != 0U));

  *sequence = sequence_end;
  return 1U;
}

void APP_Power_GetRawSnapshot(PowerRawSnapshot_t *snapshot)
{
  uint32_t primask;

  if (snapshot == NULL)
  {
    return;
  }
  primask = BSP_Critical_Enter();
  snapshot->adc_chassis_raw = power_monitor.adc_chassis_raw;
  snapshot->chassis_current_ma = power_monitor.chassis_current_ma;
  snapshot->chassis_power_w = power_monitor.chassis_power_w;
  snapshot->adc_start_count = power_monitor.adc_start_count;
  snapshot->adc_success_count = power_monitor.adc_success_count;
  snapshot->adc_consecutive_failure_count =
      power_monitor.adc_consecutive_failure_count;
  snapshot->measurement_saturation_risk =
      power_monitor.measurement_saturation_risk;
  BSP_Critical_Exit(primask);
}

void APP_Power_GetFaultSnapshot(PowerFaultSnapshot_t *snapshot)
{
  uint32_t primask;

  if (snapshot == NULL)
  {
    return;
  }
  primask = BSP_Critical_Enter();
  snapshot->power_cut_count = power_monitor.power_cut_count;
  snapshot->current_overload_count = power_monitor.current_overload_count;
  snapshot->power_cut_active = power_monitor.power_cut_active;
  snapshot->current_overload_active = power_monitor.current_overload_active;
  snapshot->adc_fault_active = power_monitor.adc_fault_active;
  snapshot->last_fault = power_monitor.last_fault;
  BSP_Critical_Exit(primask);
}

uint8_t APP_Power_GetFireEvent(uint8_t *prohibited, uint32_t *sequence)
{
  uint32_t primask;

  if ((prohibited == NULL) || (sequence == NULL))
  {
    return 0U;
  }
  primask = BSP_Critical_Enter();
  *prohibited = power_fire_prohibited;
  *sequence = power_fire_event_sequence;
  BSP_Critical_Exit(primask);
  return 1U;
}

static void APP_Power_OnChassisSample(uint16_t adc_raw)
{
  power_monitor.adc_chassis_raw = adc_raw;
  power_monitor.adc_success_count++;
  power_monitor.chassis_current_ma =
      BSP_INA180_AdcRawToMilliamp(power_monitor.adc_chassis_raw,
                                  APP_POWER_ADC_REFERENCE_MV);
  power_monitor.chassis_power_w =
      BSP_INA180_CurrentMilliampToWatt(power_monitor.chassis_current_ma,
                                       APP_POWER_BUS_VOLTAGE_MV);
  power_monitor.measurement_saturation_risk =
      (power_monitor.chassis_power_w > APP_POWER_MEASUREMENT_WARNING_W) ? 1U : 0U;

  if ((power_monitor.current_limit_ma != 0U) &&
      (power_monitor.chassis_current_ma >= power_monitor.current_limit_ma) &&
      (power_monitor.current_overload_active == 0U))
  {
    power_overcurrent_elapsed_ms += APP_POWER_ADC_SAMPLE_PERIOD_MS;
    if (power_overcurrent_elapsed_ms >= APP_POWER_OVERCURRENT_DURATION_MS)
    {
      power_monitor.current_overload_count++;
      power_monitor.last_fault = POWER_FAULT_OVERCURRENT;
      power_monitor.current_overload_active = 1U;
      power_monitor.fire_prohibited = 1U;
      APP_Power_SetFirePermission(1U);
      APP_Power_SetChassisState(POWER_SWITCH_OFF);
    }
  }
  else if (power_monitor.current_overload_active == 0U)
  {
    power_overcurrent_elapsed_ms = 0U;
  }

  power_window_sum_w += power_monitor.chassis_power_w;
  power_window_sum_ma += power_monitor.chassis_current_ma;
  if (power_monitor.chassis_power_w > power_window_peak_w)
  {
    power_window_peak_w = power_monitor.chassis_power_w;
  }
  if (power_monitor.measurement_saturation_risk != 0U)
  {
    power_window_saturation_risk = 1U;
  }
  power_window_sample_count++;

  if (power_settlement_due != 0U)
  {
    power_settlement_due = 0U;
    power_monitor.chassis_power_avg_w =
        (uint32_t)(power_window_sum_w / power_window_sample_count);
    power_monitor.chassis_current_avg_ma =
        (uint32_t)(power_window_sum_ma / power_window_sample_count);
    power_monitor.chassis_power_peak_w = power_window_peak_w;
    power_monitor.measurement_saturation_risk = power_window_saturation_risk;
    power_window_sum_w = 0U;
    power_window_sum_ma = 0U;
    power_window_peak_w = 0U;
    power_window_saturation_risk = 0U;
    power_window_sample_count = 0U;
    APP_Power_SettlePowerLimit();
    power_report_sequence++;
    power_report_snapshot.chassis_power_avg_w = power_monitor.chassis_power_avg_w;
    power_report_snapshot.chassis_power_peak_w = power_monitor.chassis_power_peak_w;
    power_report_snapshot.chassis_current_avg_ma =
        power_monitor.chassis_current_avg_ma;
    power_report_snapshot.power_limit_exceeded = power_monitor.power_limit_exceeded;
    power_report_snapshot.measurement_saturation_risk =
        power_monitor.measurement_saturation_risk;
    power_report_snapshot.power_buffer_deci_j = power_monitor.power_buffer_deci_j;
    power_report_snapshot.power_limit_w = power_monitor.power_limit_w;
    power_report_snapshot.current_limit_ma = power_monitor.current_limit_ma;
    power_report_snapshot.power_cut_active = power_monitor.power_cut_active;
    power_report_snapshot.current_overload_active =
        power_monitor.current_overload_active;
    power_report_snapshot.adc_fault_active = power_monitor.adc_fault_active;
    power_report_snapshot.fire_prohibited = power_monitor.fire_prohibited;
    power_report_snapshot.switch_chassis = power_monitor.switch_chassis;
    power_report_snapshot.switch_reserved = power_monitor.switch_reserved;
    power_report_sequence++;
    power_report_valid = 1U;
  }
}

static void APP_Power_SettlePowerLimit(void)
{
  uint32_t power_difference_w;

  power_monitor.power_limit_exceeded = 0U;

#if (APP_POWER_LIMIT_ENABLE == 0U) || (APP_POWER_BUFFER_ENABLE == 0U)
  /* 功率限制和缓冲机制暂时停用，保留结算代码供后续重新启用。 */
  (void)power_difference_w;
  return;
#endif

  if (power_monitor.chassis_power_w > power_monitor.power_limit_w)
  {
    power_difference_w = power_monitor.chassis_power_w -
                         power_monitor.power_limit_w;
    power_monitor.power_limit_exceeded = 1U;

    if (power_difference_w >= power_monitor.power_buffer_deci_j)
    {
      power_monitor.power_buffer_deci_j = 0U;
    }
    else
    {
      power_monitor.power_buffer_deci_j -= (uint16_t)power_difference_w;
    }
  }
  else
  {
    power_difference_w = power_monitor.power_limit_w -
                         power_monitor.chassis_power_w;
    if (power_difference_w >=
        (APP_POWER_BUFFER_MAX_DECI_J - power_monitor.power_buffer_deci_j))
    {
      power_monitor.power_buffer_deci_j = APP_POWER_BUFFER_MAX_DECI_J;
    }
    else
    {
      power_monitor.power_buffer_deci_j += (uint16_t)power_difference_w;
    }
  }

  if ((power_monitor.power_buffer_deci_j == 0U)
      && (power_monitor.power_limit_exceeded != 0U)
      && (power_monitor.power_cut_active == 0U))
  {
    APP_Power_SetChassisState(POWER_SWITCH_OFF);
    power_monitor.power_cut_active = 1U;
    power_monitor.power_cut_count++;
    power_monitor.last_fault = POWER_FAULT_POWER_CUT;
    power_monitor.fire_prohibited = 1U;
    APP_Power_SetFirePermission(1U);
    power_cut_remaining_ms = APP_POWER_CUT_DURATION_MS;
    APP_Power_OverLimitCallback();
  }
}

uint8_t APP_Power_SetApplicationPowerLimit(uint16_t power_limit_w)
{
  uint32_t primask;

  if ((power_limit_w == 0U) ||
      (power_limit_w > APP_POWER_APPLICATION_LIMIT_MAX_W))
  {
    return 0U;
  }
  primask = BSP_Critical_Enter();
  power_monitor.power_limit_w = power_limit_w;
  BSP_Critical_Exit(primask);
  return 1U;
}

uint8_t APP_Power_SetApplicationBufferEnergy(uint16_t buffer_energy_deci_j)
{
  uint32_t primask;

  if (buffer_energy_deci_j > APP_POWER_BUFFER_MAX_DECI_J)
  {
    return 0U;
  }
  primask = BSP_Critical_Enter();
  power_monitor.power_buffer_deci_j = buffer_energy_deci_j;
  BSP_Critical_Exit(primask);
  return 1U;
}

uint8_t APP_Power_SetSafetyCurrentLimit(uint32_t current_limit_ma)
{
  uint32_t primask;

  if (current_limit_ma > APP_POWER_CURRENT_LIMIT_MAX_MA)
  {
    return 0U;
  }
  primask = BSP_Critical_Enter();
  power_monitor.current_limit_ma = current_limit_ma;
  power_overcurrent_elapsed_ms = 0U;
  if ((current_limit_ma == 0U) ||
      (power_monitor.chassis_current_ma < current_limit_ma))
  {
    power_monitor.current_overload_active = 0U;
  }
  BSP_Critical_Exit(primask);
  return 1U;
}

uint8_t APP_Power_ForceChassisState(PowerSwitchState_t state)
{
  if ((state != POWER_SWITCH_ON) && (state != POWER_SWITCH_OFF))
  {
    return 0U;
  }
  if (state == POWER_SWITCH_ON)
  {
    if (APP_Power_CanTurnChassisOn() == 0U)
    {
      return 0U;
    }
  }
  APP_Power_SetChassisState(state);
  if (state == POWER_SWITCH_ON &&
      (power_monitor.power_cut_active == 0U) &&
      (power_monitor.current_overload_active == 0U) &&
      (power_monitor.adc_fault_active == 0U))
  {
    power_monitor.fire_prohibited = 0U;
    APP_Power_SetFirePermission(0U);
  }
  else if (state == POWER_SWITCH_OFF)
  {
    power_monitor.fire_prohibited = 1U;
    APP_Power_SetFirePermission(1U);
  }
  return 1U;
}

uint8_t APP_Power_ForceReservedState(PowerSwitchState_t state)
{
  if ((state != POWER_SWITCH_ON) && (state != POWER_SWITCH_OFF))
  {
    return 0U;
  }
  power_monitor.switch_reserved = state;
  return 1U;
}

uint8_t APP_Power_RequestChassisOn(void)
{
  if (APP_Power_CanTurnChassisOn() == 0U)
  {
    return 0U;
  }
  APP_Power_SetChassisState(POWER_SWITCH_ON);
  power_monitor.fire_prohibited = 0U;
  APP_Power_SetFirePermission(0U);
  return 1U;
}

uint8_t APP_Power_RequestReservedOn(void)
{
  return 0U;
}

__attribute__((weak)) void APP_Power_OverLimitCallback(void)
{
}
