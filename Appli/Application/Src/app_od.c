#include "app_od.h"
#include <string.h>
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "stm32n6xx_hal.h"
#include "nnlib.h"
#include "app_camera_ethernet_test.h"

#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

/* ML buffer from camera */
extern uint8_t ml_buffer[];
#define ML_WIDTH 256
#define ML_HEIGHT 256

/* Semaphore for OD task */
ID sem_od_frame_ready = -1;

/* Static AI context */
STAI_NETWORK_CONTEXT_DECLARE(od_network, STAI_OD_MODEL_CONTEXT_SIZE);

volatile bool npu_is_inferencing = false;
volatile uint32_t npu_inf_start_tick = 0;

/* IoU Helper for Non-Maximum Suppression (NMS) */
static float calculate_iou(float cx1, float cy1, float w1, float h1,
                           float cx2, float cy2, float w2, float h2) {
    float x1_min = cx1 - w1 * 0.5f;
    float y1_min = cy1 - h1 * 0.5f;
    float x1_max = cx1 + w1 * 0.5f;
    float y1_max = cy1 + h1 * 0.5f;

    float x2_min = cx2 - w2 * 0.5f;
    float y2_min = cy2 - h2 * 0.5f;
    float x2_max = cx2 + w2 * 0.5f;
    float y2_max = cy2 + h2 * 0.5f;

    float inter_x_min = (x1_min > x2_min) ? x1_min : x2_min;
    float inter_y_min = (y1_min > y2_min) ? y1_min : y2_min;
    float inter_x_max = (x1_max < x2_max) ? x1_max : x2_max;
    float inter_y_max = (y1_max < y2_max) ? y1_max : y2_max;

    float inter_w = inter_x_max - inter_x_min;
    float inter_h = inter_y_max - inter_y_min;

    if (inter_w <= 0.0f || inter_h <= 0.0f) return 0.0f;

    float inter_area = inter_w * inter_h;
    float area1 = w1 * h1;
    float area2 = w2 * h2;
    float union_area = area1 + area2 - inter_area;

    if (union_area <= 0.0f) return 0.0f;
    return inter_area / union_area;
}

LOCAL void npu_watchdog_task(INT stacd, void *exinf) {
  (void)stacd;
  (void)exinf;

  while (1) {
    tk_dly_tsk(500); // Check every 500ms
    if (npu_is_inferencing && (HAL_GetTick() - npu_inf_start_tick > 2000)) {
        PRINT("\r\n========================================\r\n");
        PRINT("[WATCHDOG] NPU HANG DETECTED! (Inference exceeded 2000ms)\r\n");
        PRINT("========================================\r\n");

        volatile uint32_t *clkctrl_regs = (volatile uint32_t *)NPU_BASE;
        PRINT("[CLKCTRL (0x580E0000)] CTRL: 0x%08X | VER: 0x%08X | BGATES: 0x%08X\r\n",
              (unsigned int)clkctrl_regs[0], (unsigned int)clkctrl_regs[1], (unsigned int)clkctrl_regs[4]);

        volatile uint32_t *intctrl_regs = (volatile uint32_t *)(NPU_BASE + 0x1000);
        PRINT("[INTCTRL (0x580E1000)] CTRL: 0x%08X | INTREG: 0x%08X | ORMSK: 0x%08X | ANDMSK: 0x%08X\r\n",
              (unsigned int)intctrl_regs[0], (unsigned int)intctrl_regs[4], 
              (unsigned int)intctrl_regs[9], (unsigned int)intctrl_regs[10]);

        volatile uint32_t *epochctrl_regs = (volatile uint32_t *)(NPU_BASE + 0x1E000);
        PRINT("[EPOCHCTRL (0x580FE000)] CTRL: 0x%08X | VER: 0x%08X | ADDR: 0x%08X | IRQ/ERR: 0x%08X\r\n",
              (unsigned int)epochctrl_regs[0], (unsigned int)epochctrl_regs[1], 
              (unsigned int)epochctrl_regs[2], (unsigned int)epochctrl_regs[3]);

        PRINT("\r\n[WATCHDOG] NVIC STATE:\r\n");
        PRINT("NVIC_ISER[1]: 0x%08X (NPU0_IRQn 53 %s)\r\n",
              (unsigned int)NVIC->ISER[1],
              (NVIC->ISER[1] & (1U << 21)) ? "ENABLED" : "DISABLED");
        PRINT("NVIC_ISPR[1]: 0x%08X (NPU0_IRQn 53 %s)\r\n",
              (unsigned int)NVIC->ISPR[1],
              (NVIC->ISPR[1] & (1U << 21)) ? "PENDING" : "IDLE");
        PRINT("========================================\r\n");
    }
  }
}

LOCAL ID tskid_npu_wdg;
LOCAL T_CTSK ctsk_npu_wdg = {
    .itskpri = 11,
    .stksz = 4096,
    .task = npu_watchdog_task,
    .tskatr = TA_HLNG | TA_RNG0,
};

LOCAL void od_task(INT stacd, void *exinf) {
  (void)stacd;
  (void)exinf;

  PRINT("[OD] Starting OD Task...\r\n");

  /* Create Semaphore */
  T_CSEM csem = {.exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1};
  sem_od_frame_ready = tk_cre_sem(&csem);
  if (sem_od_frame_ready <= 0) {
    PRINT("[OD ERROR] Failed to create OD semaphore\r\n");
    tk_ext_tsk();
    return;
  }

  /* Setup NNLIB config */
  nnlib_config_t nn_config = {
      .network = od_network,
      .external_weights_addr = (void*)0x71000000 /* NOR Flash address where model weights are stored */
  };

  /* Initialize Hardware and Model */
  if (!nnlib_init(&nn_config)) {
    PRINT("[OD ERROR] NNLIB Init Failed\r\n");
    tk_ext_tsk();
    return;
  }
  PRINT("[OD] Model Initialized successfully!\r\n");

  uint32_t inf_count = 0;
  PRINT("[OD] Entering main inference loop...\r\n");

  while (1) {
    /* Drain any stale camera frame signals accumulated during inference */
    while (tk_wai_sem(sem_od_frame_ready, 1, TMO_POL) == E_OK);

    /* Wait for a fresh camera frame (1-second timeout so it never hangs silently) */
    ER sem_err = tk_wai_sem(sem_od_frame_ready, 1, 1000);
    if (sem_err != E_OK) {
      PRINT("[OD] Waiting for camera frame...\r\n");
      continue;
    }

    /* Copy frame to NPU input */
    if (!nnlib_set_input(&nn_config, ml_buffer, ML_WIDTH * ML_HEIGHT * 3)) {
        PRINT("[OD ERROR] Failed to set input buffer\r\n");
        continue;
    }

    /* Run Inference */
    uint32_t inf_ms = 0;
    npu_inf_start_tick = HAL_GetTick();
    npu_is_inferencing = true;
    bool inf_ret = nnlib_run_inference(&nn_config, &inf_ms);
    npu_is_inferencing = false;

    if (!inf_ret) {
        PRINT("[OD ERROR] Inference Failed or Hanged!\r\n");
        continue;
    }

    inf_count++;

    /* Get, Filter with NMS and Stream results */
    float *bboxes = NULL;
    if (nnlib_get_output(&nn_config, (void**)&bboxes) && bboxes != NULL) {
        #define MAX_CANDIDATES 32
        DetectionBox_t candidates[MAX_CANDIDATES];
        bool suppressed[MAX_CANDIDATES] = {false};
        uint8_t num_candidates = 0;
        float max_conf = 0.0f;
        uint16_t best_anchor = 0;

        for (uint16_t i = 0; i < 1344; i++) {
            float conf = bboxes[5376 + i];
            if (conf > max_conf) {
                max_conf = conf;
                best_anchor = i;
            }
            if (conf >= 0.30f && num_candidates < MAX_CANDIDATES) {
                candidates[num_candidates].cx = bboxes[0 * 1344 + i];
                candidates[num_candidates].cy = bboxes[1 * 1344 + i];
                candidates[num_candidates].w  = bboxes[2 * 1344 + i];
                candidates[num_candidates].h  = bboxes[3 * 1344 + i];
                candidates[num_candidates].conf = conf;
                candidates[num_candidates].class_id = 0;
                candidates[num_candidates].reserved = 0;
                num_candidates++;
            }
        }

        /* Sort candidates by confidence descending */
        for (uint8_t i = 0; i < num_candidates; i++) {
            for (uint8_t j = i + 1; j < num_candidates; j++) {
                if (candidates[j].conf > candidates[i].conf) {
                    DetectionBox_t tmp = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = tmp;
                }
            }
        }

        /* Non-Maximum Suppression (IoU > 0.45 threshold) */
        DetectionBox_t detected_boxes[OD_MAX_STREAM_BOXES];
        uint8_t num_detected = 0;

        for (uint8_t i = 0; i < num_candidates; i++) {
            if (suppressed[i]) continue;

            if (num_detected < OD_MAX_STREAM_BOXES) {
                detected_boxes[num_detected++] = candidates[i];
            }

            for (uint8_t j = i + 1; j < num_candidates; j++) {
                if (suppressed[j]) continue;
                float iou = calculate_iou(
                    candidates[i].cx, candidates[i].cy, candidates[i].w, candidates[i].h,
                    candidates[j].cx, candidates[j].cy, candidates[j].w, candidates[j].h
                );
                if (iou > 0.45f) {
                    suppressed[j] = true;
                }
            }
        }

        if (num_detected > 0) {
            if (inf_count % 10 == 0) {
                PRINT(">>> [PERSON / OBJECT DETECTED!] Count: %u | Conf: %d%% | Box: [cx=%d cy=%d w=%d h=%d] | Infer: %lu ms\r\n",
                      (unsigned int)num_detected,
                      (int)(detected_boxes[0].conf * 100.0f),
                      (int)(detected_boxes[0].cx * 1000.0f),
                      (int)(detected_boxes[0].cy * 1000.0f),
                      (int)(detected_boxes[0].w * 1000.0f),
                      (int)(detected_boxes[0].h * 1000.0f),
                      (unsigned long)inf_ms);
            }
        } else if (inf_count % 10 == 0) {
            PRINT("--- [NO DETECTION] Max Conf: %d%% (Threshold: 30%%) | Infer: %lu ms\r\n",
                  (int)(max_conf * 100.0f), (unsigned long)inf_ms);
        }

        /* Update streamer with latest detection metadata (lightweight, asynchronous) */
        Ethernet_Streamer_UpdateDetections(
            inf_count, inf_ms, num_detected, detected_boxes
        );
    }
  }
}

LOCAL ID tskid_od;
LOCAL T_CTSK ctsk_od = {
    .itskpri = 12,
    .stksz = 8192,
    .task = od_task,
    .tskatr = TA_HLNG | TA_RNG0,
};

void start_od_task(void) {
  tskid_od = tk_cre_tsk(&ctsk_od);
  if (tskid_od > 0) {
    tk_sta_tsk(tskid_od, 0);
  } else {
    PRINT("[OD ERROR] Failed to create OD task: %d\r\n", tskid_od);
  }

  tskid_npu_wdg = tk_cre_tsk(&ctsk_npu_wdg);
  if (tskid_npu_wdg > 0) {
    tk_sta_tsk(tskid_npu_wdg, 0);
  } else {
    PRINT("[OD ERROR] Failed to create Watchdog task\r\n");
  }
}
