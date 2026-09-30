 /**
 ******************************************************************************
 * @file    app_camerapipeline.c
 * @author  GPM Application Team
 *
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#include "cmw_camera.h"
#include "app_camerapipeline.h"
#include <tm/tmonitor.h>
#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)
#if defined(USE_IMX335_SENSOR)
  #define GAMMA_CONVERSION 0
#elif defined(USE_VD66GY_SENSOR)
  #define GAMMA_CONVERSION 0
#elif defined(USE_VD55G1_SENSOR)
  #define GAMMA_CONVERSION 0
#else
  #define GAMMA_CONVERSION 0
#endif

#define NN_WIDTH  256
#define NN_HEIGHT 256
#define NN_BPP    3
#define SCREEN_HEIGHT 480
#define SCREEN_WIDTH  800

/* Leave the driver use the default resolution */
#define CAMERA_WIDTH 0
#define CAMERA_HEIGHT 0
#define CAMERA_FPS 30

extern int32_t cameraFrameReceived;

static void DCMIPP_PipeInitDisplay(CMW_CameraInit_t *camConf, uint32_t *bg_width, uint32_t *bg_height)
{
  CMW_Aspect_Ratio_Mode_t aspect_ratio;
  CMW_DCMIPP_Conf_t dcmipp_conf = {0};
  int ret;

  aspect_ratio = CMW_Aspect_ratio_crop;

  int lcd_bg_width;
  int lcd_bg_height;

  lcd_bg_height = (camConf->height <= SCREEN_HEIGHT) ? camConf->height : SCREEN_HEIGHT;

  lcd_bg_width = (camConf->height <= SCREEN_HEIGHT) ? camConf->height : SCREEN_HEIGHT;

  *bg_width = lcd_bg_width;
  *bg_height = lcd_bg_height;

  dcmipp_conf.output_width = lcd_bg_width;
  dcmipp_conf.output_height = lcd_bg_height;
  dcmipp_conf.output_format = DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1;
  dcmipp_conf.output_bpp = 2;
  dcmipp_conf.mode = aspect_ratio;
  dcmipp_conf.enable_gamma_conversion = GAMMA_CONVERSION;
  uint32_t pitch;
  ret = CMW_CAMERA_SetPipeConfig(DCMIPP_PIPE1, &dcmipp_conf, &pitch);
  if (ret != HAL_OK) {
    PRINT("[CAMERA ERROR] SetPipeConfig PIPE1 failed: %d\r\n", ret);
  }
  if (dcmipp_conf.output_width * dcmipp_conf.output_bpp != pitch) {
    PRINT("[CAMERA WARNING] PIPE1 Pitch mismatch!\r\n");
  }
}

static void DCMIPP_PipeInitNn(uint32_t *pitch)
{
  CMW_Aspect_Ratio_Mode_t aspect_ratio;
  CMW_DCMIPP_Conf_t dcmipp_conf;
  int ret;

  aspect_ratio = CMW_Aspect_ratio_crop;

  dcmipp_conf.output_width = NN_WIDTH;
  dcmipp_conf.output_height = NN_HEIGHT;
  dcmipp_conf.output_format = DCMIPP_PIXEL_PACKER_FORMAT_RGB888_YUV444_1;
  dcmipp_conf.output_bpp = NN_BPP;
  dcmipp_conf.mode = aspect_ratio;
  dcmipp_conf.enable_swap = 1; /* Enable Red/Blue swap so memory receives true RGB888 */
  dcmipp_conf.enable_gamma_conversion = GAMMA_CONVERSION;
  ret = CMW_CAMERA_SetPipeConfig(DCMIPP_PIPE2, &dcmipp_conf, pitch);
  if (ret != HAL_OK) {
    PRINT("[CAMERA ERROR] SetPipeConfig PIPE2 failed: %d\r\n", ret);
  }
}

/**
* @brief Init the camera and the 2 DCMIPP pipes
* @param lcd_bg_width display width
* @param lcd_bg_height display height
* @param pitch_nn output pitch computed by the CMW
*/
void CameraPipeline_Init(uint32_t *lcd_bg_width, uint32_t *lcd_bg_height, uint32_t *pitch_nn)
{
  int ret;
  CMW_CameraInit_t cam_conf;

  cam_conf.width = CAMERA_WIDTH;
  cam_conf.height = CAMERA_HEIGHT;
  cam_conf.fps = CAMERA_FPS;
  cam_conf.pixel_format = 0; /* Default; Not implemented yet */
  cam_conf.anti_flicker = 0;
  cam_conf.mirror_flip = CMW_MIRRORFLIP_NONE;

  ret = CMW_CAMERA_Init(&cam_conf);
  if (ret != CMW_ERROR_NONE) {
    PRINT("[CAMERA ERROR] CMW_CAMERA_Init failed: %d\r\n", ret);
  }
  
  DCMIPP_PipeInitDisplay(&cam_conf, lcd_bg_width, lcd_bg_height);
  DCMIPP_PipeInitNn(pitch_nn);
}

void CameraPipeline_DeInit(void)
{
  int ret;
  ret = CMW_CAMERA_DeInit();
  if (ret != CMW_ERROR_NONE) {
    PRINT("[CAMERA ERROR] CMW_CAMERA_DeInit failed: %d\r\n", ret);
  }
}

void CameraPipeline_DisplayPipe_Start(uint8_t *display_pipe_dst, uint32_t cam_mode)
{
  int ret;
  ret = CMW_CAMERA_Start(DCMIPP_PIPE1, display_pipe_dst, cam_mode);
  if (ret != CMW_ERROR_NONE) {
    PRINT("[CAMERA ERROR] CMW_CAMERA_Start PIPE1 failed: %d\r\n", ret);
  }
}

void CameraPipeline_NNPipe_Start(uint8_t *nn_pipe_dst, uint32_t cam_mode)
{
  int ret;

  ret = CMW_CAMERA_Start(DCMIPP_PIPE2, nn_pipe_dst, cam_mode);
  if (ret != CMW_ERROR_NONE) {
    PRINT("[CAMERA ERROR] CMW_CAMERA_Start PIPE2 failed: %d\r\n", ret);
  }
}

void CameraPipeline_DisplayPipe_Stop()
{
  int ret;
  ret = CMW_CAMERA_Suspend(DCMIPP_PIPE1);
  if (ret != CMW_ERROR_NONE) {
    PRINT("[CAMERA ERROR] CMW_CAMERA_Suspend PIPE1 failed: %d\r\n", ret);
  }
}

void CameraPipeline_IspUpdate(void)
{
  int ret = CMW_ERROR_NONE;
  ret = CMW_CAMERA_Run();
  if (ret != CMW_ERROR_NONE) {
    PRINT("[CAMERA ERROR] CMW_CAMERA_Run failed: %d\r\n", ret);
  }
}


