#ifndef BSP_ADC_H
#define BSP_ADC_H

#include <stdint.h>

typedef enum
{
  BSP_ADC_STATUS_OK = 0U,
  BSP_ADC_STATUS_BUSY,
  BSP_ADC_STATUS_ERROR
} BSP_AdcStatus_t;

typedef void (*BSP_AdcAmmoCallback_t)(uint16_t adc_raw,
                                      uint32_t sample_tick);

BSP_AdcStatus_t BSP_Adc_CalibrateAmmo(void);
BSP_AdcStatus_t BSP_Adc_StartAmmo(void);
void BSP_Adc_RegisterAmmoCallback(BSP_AdcAmmoCallback_t callback);

#endif /* BSP_ADC_H */
