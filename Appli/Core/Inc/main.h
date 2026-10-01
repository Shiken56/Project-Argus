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

#if defined ( __ICCARM__ )
#  define CMSE_NS_CALL  __cmse_nonsecure_call
#  define CMSE_NS_ENTRY __cmse_nonsecure_entry
#else
#  define CMSE_NS_CALL  __attribute((cmse_nonsecure_call))
#  define CMSE_NS_ENTRY __attribute((cmse_nonsecure_entry))
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32n6xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* Function pointer declaration in non-secure*/
#if defined ( __ICCARM__ )
typedef void (CMSE_NS_CALL *funcptr)(void);
#else
typedef void CMSE_NS_CALL (*funcptr)(void);
#endif

/* typedef for non-secure callback functions */
typedef funcptr funcptr_NS;

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
#define OSC_IN_Pin GPIO_PIN_0
#define OSC_IN_GPIO_Port GPIOH
#define I2C1_SDA_Pin GPIO_PIN_1
#define I2C1_SDA_GPIO_Port GPIOC
#define OSC_OUT_Pin GPIO_PIN_1
#define OSC_OUT_GPIO_Port GPIOH
#define VCP_TX_Pin GPIO_PIN_5
#define VCP_TX_GPIO_Port GPIOE
#define I2C1_SCL_Pin GPIO_PIN_9
#define I2C1_SCL_GPIO_Port GPIOH
#define VCP_RX_Pin GPIO_PIN_6
#define VCP_RX_GPIO_Port GPIOE
#define HEXASPI_IO_7_Pin GPIO_PIN_7
#define HEXASPI_IO_7_GPIO_Port GPIOP
#define HEXASPI_IO_6_Pin GPIO_PIN_6
#define HEXASPI_IO_6_GPIO_Port GPIOP
#define HEXASPI_IO_0_Pin GPIO_PIN_0
#define HEXASPI_IO_0_GPIO_Port GPIOP
#define HEXASPI_IO_4_Pin GPIO_PIN_4
#define HEXASPI_IO_4_GPIO_Port GPIOP
#define HEXASPI_IO_1_Pin GPIO_PIN_1
#define HEXASPI_IO_1_GPIO_Port GPIOP
#define HEXASPI_IO_5_Pin GPIO_PIN_5
#define HEXASPI_IO_5_GPIO_Port GPIOP
#define HEXASPI_IO_3_Pin GPIO_PIN_3
#define HEXASPI_IO_3_GPIO_Port GPIOP
#define HEXASPI_IO_2_Pin GPIO_PIN_2
#define HEXASPI_IO_2_GPIO_Port GPIOP
#define LED_GREEN_Pin GPIO_PIN_1
#define LED_GREEN_GPIO_Port GPIOO
#define HEXASPI_DQS0_Pin GPIO_PIN_2
#define HEXASPI_DQS0_GPIO_Port GPIOO
#define ETH_RXD2_Pin GPIO_PIN_8
#define ETH_RXD2_GPIO_Port GPIOF
#define HEXASPI_CLK_Pin GPIO_PIN_4
#define HEXASPI_CLK_GPIO_Port GPIOO
#define ETH_RXD3_Pin GPIO_PIN_9
#define ETH_RXD3_GPIO_Port GPIOF
#define ETH_TXD3_Pin GPIO_PIN_4
#define ETH_TXD3_GPIO_Port GPIOG
#define ETH_GTX_CLK_Pin GPIO_PIN_0
#define ETH_GTX_CLK_GPIO_Port GPIOF
#define UCPD1_VSENSE_Pin GPIO_PIN_11
#define UCPD1_VSENSE_GPIO_Port GPIOA
#define LED_RED_Pin GPIO_PIN_10
#define LED_RED_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
