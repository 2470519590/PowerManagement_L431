#ifndef BSP_INA180_H
#define BSP_INA180_H

#include <stdint.h>

uint32_t BSP_INA180_AdcRawToMilliamp(uint16_t adc_raw,
                                     uint32_t adc_reference_mv);
uint32_t BSP_INA180_CurrentMilliampToWatt(uint32_t current_ma,
                                          uint32_t bus_voltage_mv);

#endif /* BSP_INA180_H */
