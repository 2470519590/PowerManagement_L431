#include "bsp_adc.h"
#include "adc.h"

static BSP_AdcAmmoCallback_t ammo_callback;

static BSP_AdcStatus_t BSP_Adc_ConvertStatus(HAL_StatusTypeDef status)
{
  if (status == HAL_OK)
  {
    return BSP_ADC_STATUS_OK;
  }
  if (status == HAL_BUSY)
  {
    return BSP_ADC_STATUS_BUSY;
  }
  return BSP_ADC_STATUS_ERROR;
}

BSP_AdcStatus_t BSP_Adc_CalibrateAmmo(void)
{
  return BSP_Adc_ConvertStatus(
      HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED));
}

BSP_AdcStatus_t BSP_Adc_StartAmmo(void)
{
  return BSP_Adc_ConvertStatus(HAL_ADC_Start_IT(&hadc1));
}

void BSP_Adc_RegisterAmmoCallback(BSP_AdcAmmoCallback_t callback)
{
  ammo_callback = callback;
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc == NULL) || (hadc->Instance != ADC1) || (ammo_callback == NULL))
  {
    return;
  }

  ammo_callback((uint16_t)HAL_ADC_GetValue(hadc), HAL_GetTick());
}
