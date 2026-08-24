#include "app_power.h"
#include "bsp_adc.h"
#include "bsp_power.h"
#include <stdio.h>
#include "app_power_config.h"
#include "bsp_ina180.h"

volatile PowerMonitorState_t power_monitor =
{
  .switch_ammo = POWER_SWITCH_ON
};

static uint32_t power_sample_elapsed_ms;
static uint64_t power_window_sum_w;
static uint32_t power_window_peak_w;
static uint32_t power_window_sample_count;
static volatile uint8_t power_report_pending;

static void APP_Power_OnAmmoSample(uint16_t adc_raw, uint32_t sample_tick);

static void APP_Power_SetAmmoState(PowerSwitchState_t state)
{
  if (state == POWER_SWITCH_ON)
  {
    BSP_Power_AmmoOn();
  }
  else
  {
    BSP_Power_AmmoOff();
  }
  power_monitor.switch_ammo = state;
}

void APP_Power_Init(void)
{
  BSP_Power_Init();
  APP_Power_SetAmmoState(POWER_SWITCH_ON);
  power_monitor.adc_attempt_count = 0U;
  power_monitor.adc_success_count = 0U;
  power_monitor.adc_status = BSP_ADC_STATUS_OK;
  power_monitor.adc_ammo_raw = 0U;
  power_monitor.ammo_current_ma = 0U;
  power_monitor.ammo_power_w = 0U;
  power_monitor.ammo_power_avg_w = 0U;
  power_monitor.ammo_power_peak_w = 0U;
  power_monitor.power_limit_latched = 0U;
  power_monitor.adc_last_sample_tick = 0U;
  power_sample_elapsed_ms = 0U;
  power_window_sum_w = 0U;
  power_window_peak_w = 0U;
  power_window_sample_count = 0U;
  power_report_pending = 0U;

  BSP_Adc_RegisterAmmoCallback(APP_Power_OnAmmoSample);
  if (BSP_Adc_CalibrateAmmo() != BSP_ADC_STATUS_OK)
  {
    Error_Handler();
  }
}

void HAL_SYSTICK_Callback(void)
{
  power_sample_elapsed_ms++;
  if (power_sample_elapsed_ms >= APP_POWER_ADC_SAMPLE_PERIOD_MS)
  {
    power_monitor.adc_attempt_count++;
    power_monitor.adc_status = BSP_Adc_StartAmmo();
    power_sample_elapsed_ms = 0U;
  }
}

void APP_Power_ReportTask(void)
{
  if (power_report_pending == 0U)
  {
    return;
  }

  power_report_pending = 0U;
  printf("Pavg=%lu W Pmax=%lu W I=%lu mA\r\n",
         (unsigned long)power_monitor.ammo_power_avg_w,
         (unsigned long)power_monitor.ammo_power_peak_w,
         (unsigned long)power_monitor.ammo_current_ma);
}

void APP_Power_Task(void)
{
  APP_Power_ReportTask();
}

static void APP_Power_OnAmmoSample(uint16_t adc_raw, uint32_t sample_tick)
{
  power_monitor.adc_status = BSP_ADC_STATUS_OK;
  power_monitor.adc_success_count++;
  power_monitor.adc_last_sample_tick = sample_tick;
  power_monitor.adc_ammo_raw = adc_raw;
  power_monitor.ammo_current_ma =
      BSP_INA180_AdcRawToMilliamp(power_monitor.adc_ammo_raw,
                                  APP_POWER_ADC_REFERENCE_MV);
  power_monitor.ammo_power_w =
      BSP_INA180_CurrentMilliampToWatt(power_monitor.ammo_current_ma,
                                       APP_POWER_BUS_VOLTAGE_MV);

  power_window_sum_w += power_monitor.ammo_power_w;
  if (power_monitor.ammo_power_w > power_window_peak_w)
  {
    power_window_peak_w = power_monitor.ammo_power_w;
  }
  power_window_sample_count++;

  if ((APP_POWER_LIMIT_W > 0U)
      && (power_monitor.ammo_power_w > APP_POWER_LIMIT_W)
      && (power_monitor.power_limit_latched == 0U))
  {
    power_monitor.power_limit_latched = 1U;
    APP_Power_OverLimitCallback();
  }

  if (power_window_sample_count >= APP_POWER_WINDOW_SAMPLES)
  {
    power_monitor.ammo_power_avg_w =
        (uint32_t)(power_window_sum_w / APP_POWER_WINDOW_SAMPLES);
    power_monitor.ammo_power_peak_w = power_window_peak_w;
    power_window_sum_w = 0U;
    power_window_peak_w = 0U;
    power_window_sample_count = 0U;
    power_report_pending = 1U;
  }
}

__weak void APP_Power_OverLimitCallback(void)
{
}
