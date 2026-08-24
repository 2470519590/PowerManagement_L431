/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define AMMO_U_Pin GPIO_PIN_5
#define AMMO_U_GPIO_Port GPIOA
#define GIMBAL_U_Pin GPIO_PIN_6
#define GIMBAL_U_GPIO_Port GPIOA
#define CHASSIS_U_Pin GPIO_PIN_7
#define CHASSIS_U_GPIO_Port GPIOA
#define MINI_PC_U_Pin GPIO_PIN_4
#define MINI_PC_U_GPIO_Port GPIOC
#define INT_MINI_PC_Pin GPIO_PIN_13
#define INT_MINI_PC_GPIO_Port GPIOB
#define INT_CHASSIS_Pin GPIO_PIN_14
#define INT_CHASSIS_GPIO_Port GPIOB
#define INT_GIMBAL_Pin GPIO_PIN_15
#define INT_GIMBAL_GPIO_Port GPIOB
#define INT_AMMO_Pin GPIO_PIN_6
#define INT_AMMO_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
