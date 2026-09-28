/**
  ******************************************************************************
  * @file    fx_model.h
  * @author  STEdgeAI
  * @date    2026-09-28 18:55:10
  * @brief   Minimal description of the generated c-implemention of the network
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  ******************************************************************************
  */
#ifndef LL_ATON_FX_MODEL_H
#define LL_ATON_FX_MODEL_H

/******************************************************************************/
#define LL_ATON_FX_MODEL_C_MODEL_NAME        "fx_model"
#define LL_ATON_FX_MODEL_ORIGIN_MODEL_NAME   "fx_model"

/************************** USER ALLOCATED IOs ********************************/
// No user allocated inputs
// No user allocated outputs

/************************** INPUTS ********************************************/
#define LL_ATON_FX_MODEL_IN_NUM        (1)    // Total number of input buffers
// Input buffer 1 -- Input_0_out_0
#define LL_ATON_FX_MODEL_IN_1_ALIGNMENT   (32)
#define LL_ATON_FX_MODEL_IN_1_SIZE_BYTES  (98304)

/************************** OUTPUTS *******************************************/
#define LL_ATON_FX_MODEL_OUT_NUM        (1)    // Total number of output buffers
// Output buffer 1 -- Quantize_925_out_0
#define LL_ATON_FX_MODEL_OUT_1_ALIGNMENT   (32)
#define LL_ATON_FX_MODEL_OUT_1_SIZE_BYTES  (128)

#endif /* LL_ATON_FX_MODEL_H */
