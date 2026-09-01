#ifndef BSP_ADC_H
#define BSP_ADC_H

#include <stdint.h>

typedef void (*BSP_AdcAmmoCallback_t)(uint16_t adc_raw);

typedef enum
{
  BSP_ADC_STATUS_OK = 0U,
  BSP_ADC_STATUS_BUSY,
  BSP_ADC_STATUS_ERROR
} BSP_AdcStatus_t;

uint8_t BSP_Adc_CalibrateAmmo(void);
BSP_AdcStatus_t BSP_Adc_StartAmmo(void);
void BSP_Adc_RegisterAmmoCallback(BSP_AdcAmmoCallback_t callback);

#endif /* BSP_ADC_H */
