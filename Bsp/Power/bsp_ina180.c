#include "bsp_ina180.h"

#define INA180_ADC_FULL_SCALE       4095UL
#define INA180_GAIN                 50UL
#define INA180_SHUNT_RESISTOR_UOHM  4000UL

uint32_t BSP_INA180_AdcRawToMilliamp(uint16_t adc_raw,
                                     uint32_t adc_reference_mv)
{
  uint32_t vout_mv = (uint32_t)(((uint64_t)adc_raw *
                                 (uint64_t)adc_reference_mv) /
                                INA180_ADC_FULL_SCALE);

  /* I(mA) = VOUT(mV) / (50 * 4 mOhm) = VOUT(mV) * 5. */
  return (uint32_t)(((uint64_t)vout_mv * 1000000ULL) /
                    ((uint64_t)INA180_GAIN *
                     (uint64_t)INA180_SHUNT_RESISTOR_UOHM));
}

uint32_t BSP_INA180_CurrentMilliampToWatt(uint32_t current_ma,
                                          uint32_t bus_voltage_mv)
{
  return (uint32_t)(((uint64_t)current_ma * (uint64_t)bus_voltage_mv) /
                    1000000ULL);
}
