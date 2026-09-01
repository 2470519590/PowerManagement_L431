#include "bsp_adc.h"
#include "adc.h"

static BSP_AdcAmmoCallback_t ammo_callback;

uint8_t BSP_Adc_CalibrateAmmo(void)
{
  return (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) == HAL_OK)
             ? 1U
             : 0U;
}

BSP_AdcStatus_t BSP_Adc_StartAmmo(void)
{
  HAL_StatusTypeDef status = HAL_ADC_Start_IT(&hadc1);

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

  ammo_callback((uint16_t)HAL_ADC_GetValue(hadc));
}
