#include "app_camera.h"
#include "app_camerapipeline.h"
#include "cmw_camera.h"
#include "stm32n6xx_hal.h"
#include "app_camera_ethernet_test.h"
#include <tm/tmonitor.h>

#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

#define ML_WIDTH 256
#define ML_HEIGHT 256

/* TRON Semaphore ID */
ID sem_camera_pipe0_ready;

/* Allocate RAM buffer for the camera DMA (aligned for 32-byte cache line ops)
 */
__attribute__((aligned(32))) uint8_t ml_buffer[ML_WIDTH * ML_HEIGHT * 3];

__attribute__((aligned(32))) __attribute__((
    section(".psram_bss"))) uint8_t display_buffer[800 * 480 * 2];

/* This callback is fired by the Middleware when a frame is ready */
int CMW_CAMERA_PIPE_FrameEventCallback(uint32_t pipe) {
  if (pipe == DCMIPP_PIPE2) {
    /* Wake up the sleeping TRON task */
    tk_sig_sem(sem_camera_pipe0_ready, 1);
  }
  return 0;
}

void camera_task(INT stacd, void *exinf) {
  (void)stacd;
  (void)exinf;

  PRINT("\r\n========================================\r\n");
  PRINT("[CAMERA] Starting Camera Task...\r\n");
  PRINT("========================================\r\n");

  /* 1. Create TRON Semaphore */
  T_CSEM csem = {.exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1};
  sem_camera_pipe0_ready = tk_cre_sem(&csem);
  if (sem_camera_pipe0_ready <= 0) {
    PRINT("[CAMERA] ERROR: Failed to create TRON semaphore (err: %ld)\r\n",
          (long)sem_camera_pipe0_ready);
    tk_ext_tsk();
    return;
  }

  /* 0. PSRAM Read/Write Integrity Test */
  PRINT("[PSRAM] Testing PSRAM at 0x90000000...\r\n");
  volatile uint32_t *p_test = (volatile uint32_t *)0x90000000;
  uint32_t test_patterns[4] = {0x12345678, 0xDEADBEEF, 0x55AA55AA, 0xCAFEBABE};
  int psram_ok = 1;
  for (int i = 0; i < 4; i++) {
    p_test[i] = test_patterns[i];
  }
  for (int i = 0; i < 4; i++) {
    uint32_t read_val = p_test[i];
    if (read_val != test_patterns[i]) {
      PRINT("[PSRAM ERROR] Offset %d: wrote 0x%08lX, read back 0x%08lX!\r\n",
            i * 4, test_patterns[i], read_val);
      psram_ok = 0;
    }
  }
  if (psram_ok) {
    PRINT("[PSRAM SUCCESS] PSRAM read/write verified successfully!\r\n");
  } else {
    PRINT("[PSRAM ERROR] PSRAM read/write verification FAILED!\r\n");
  }

  /* Initialize UDP Video Streamer */
  // PRINT("[CAMERA] Initializing UDP Streamer...\r\n");
  // #include "app_camera_ethernet_test.h"
  // Ethernet_Streamer_Init();

  uint32_t lcd_bg_width, lcd_bg_height, pitch_nn;
  PRINT("[CAMERA] Initializing Camera Pipeline (reference "
        "implementation)...\r\n");
  CameraPipeline_Init(&lcd_bg_width, &lcd_bg_height, &pitch_nn);
  PRINT("[CAMERA] Pipeline Initialized. NN Pitch: %lu\r\n", pitch_nn);

  /* Fill buffer with a specific pattern (0xAA) to test if DMA is overwriting it
   */
  memset((void *)ml_buffer, 0xAA, sizeof(ml_buffer));
  memset((void *)display_buffer, 0xAA, sizeof(display_buffer));
  
  CameraPipeline_DisplayPipe_Start(display_buffer, CMW_MODE_CONTINUOUS);
  CameraPipeline_NNPipe_Start(ml_buffer, CMW_MODE_CONTINUOUS);

  /* Set nominal indoor exposure (25 ms) and sensor gain (18 dB) */
  CMW_CAMERA_SetExposure(25000);
  CMW_CAMERA_SetGain(18000);
  PRINT("[CAMERA] Set nominal exposure: 25000 us (25 ms), gain: 18000 mdB (18 dB)\r\n");

  PRINT("[CAMERA] Streaming started on Pipe 1 & 2! Waiting for frames...\r\n\r\n");

  uint32_t frame_count = 0;
  uint32_t last_tick = HAL_GetTick();

  while (1) {
    /* Go to sleep until the ISR signals the semaphore */
    ER wait_ret =
        tk_wai_sem(sem_camera_pipe0_ready, 1, 1000); // 1-second timeout
    if (wait_ret == E_TMOUT) {
      extern volatile uint32_t dcmipp_irq_count;
      extern volatile uint32_t csi_irq_count;
      PRINT("[CAMERA WARNING] No frame received in last 1000ms! Checking "
            "ISP... | IRQ: DCMIPP=%lu CSI=%lu\r\n",
            dcmipp_irq_count, csi_irq_count);

      /* Dump HW Registers */
      extern DCMIPP_HandleTypeDef hcamera_dcmipp;
      DCMIPP_TypeDef *dcmipp = hcamera_dcmipp.Instance;
      CSI_TypeDef *csi =
          (CSI_TypeDef *)0x540C0000; // CSI_BASE_NS is 0x540C0000 according to
                                     // memory map (we can just use CSI_NS but
                                     // let's be safe if it's undefined)
#if defined(CSI_NS)
      csi = CSI_NS;
#endif
      PRINT("  [DCMIPP] CMCR: 0x%08lX | CMSR1: 0x%08lX | CMSR2: 0x%08lX | CMIER: 0x%08lX\r\n",
            dcmipp->CMCR, dcmipp->CMSR1, dcmipp->CMSR2, dcmipp->CMIER);
      PRINT("  [DCMIPP] P1SR: 0x%08lX | P2SR: 0x%08lX | P2IER: 0x%08lX\r\n",
            dcmipp->P1SR, dcmipp->P2SR, dcmipp->P2IER);
      PRINT("  [CSI] CR: 0x%08lX | SR0: 0x%08lX | SR1: 0x%08lX | IER0: 0x%08lX | IER1: "
            "0x%08lX\r\n",
            csi->CR, csi->SR0, csi->SR1, csi->IER0, csi->IER1);
      PRINT("  [CSI] ERR1: 0x%08lX | ERR2: 0x%08lX\r\n", csi->ERR1, csi->ERR2);

      // CMW_CAMERA_Run();
      continue;
    }

    /* Invalidate Cache to ensure CPU reads fresh DMA data from RAM */
    SCB_InvalidateDCache_by_Addr((uint32_t *)ml_buffer, sizeof(ml_buffer));
    SCB_InvalidateDCache_by_Addr((uint32_t *)display_buffer, sizeof(display_buffer));

    /* Send frame over Ethernet at camera frame rate (256x256 RGB888 -> RGB565) */
    Ethernet_Streamer_SendVideoFrame(ml_buffer, ML_WIDTH, ML_HEIGHT, 3, frame_count);

    extern ID sem_od_frame_ready;
    if (sem_od_frame_ready > 0) {
      tk_sig_sem(sem_od_frame_ready, 1);
    }

    /* Background process for Auto-Exposure commented out to prevent 0-exposure collapse on Pipe 2 */
    // CMW_CAMERA_Run();

    frame_count++;

    /* Every 30 frames (~1 sec @ 30 FPS): log statistics & buffer pixel sample
     */
    if (frame_count % 30 == 0) {
      uint32_t now = HAL_GetTick();
      uint32_t elapsed_ms = now - last_tick;
      last_tick = now;
      uint32_t approx_fps = (elapsed_ms > 0) ? (30000 / elapsed_ms) : 0;

      /* Sample middle pixel (R, G, B) */
      uint32_t mid_idx = ((ML_HEIGHT / 2) * ML_WIDTH + (ML_WIDTH / 2)) * 3;
      uint8_t r = ml_buffer[mid_idx];
      uint8_t g = ml_buffer[mid_idx + 1];
      uint8_t b = ml_buffer[mid_idx + 2];

      extern volatile uint32_t dcmipp_irq_count;
      extern volatile uint32_t csi_irq_count;
      extern DCMIPP_HandleTypeDef hcamera_dcmipp;
      PRINT("[CAMERA] Frames: %lu | Rate: ~%lu FPS | Center Pixel RGB: (%3u, "
            "%3u, %3u) | IRQ: DCMIPP=%lu CSI=%lu\r\n",
            (unsigned long)frame_count, (unsigned long)approx_fps, r, g, b,
            dcmipp_irq_count, csi_irq_count);
      PRINT("  -> ml_buffer addr: %p | P2PPM0AR1: 0x%08lX | P2STM0AR: 0x%08lX "
            "| P2SR: 0x%08lX\r\n",
            ml_buffer, (unsigned long)hcamera_dcmipp.Instance->P2PPM0AR1,
            (unsigned long)hcamera_dcmipp.Instance->P2STM0AR,
            (unsigned long)hcamera_dcmipp.Instance->P2SR);
    }
  }
}
