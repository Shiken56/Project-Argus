#include "app_od.h"
#include <string.h>
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "stm32n6xx_hal.h"
#include "nnlib.h"
#include "stai_od_model.h"
#include "app_camera_ethernet_test.h"
#include "app_image_utils.h"
#include "app_fx.h"

#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

/* ML buffer from camera */
extern uint8_t ml_buffer[];
#define ML_WIDTH 256
#define ML_HEIGHT 256

/* =========================================================================
 * Shared RTOS Resources (used by both od_task and fx_task)
 * ========================================================================= */

/* Semaphore for OD task — signaled by camera ISR */
ID sem_od_frame_ready = -1;

/* NPU mutex semaphore (count=1) — protects the single NPU hardware.
 * Both od_task and fx_task must acquire this before calling nnlib_run_inference(). */
ID sem_npu = -1;

/* Mailbox for passing detection jobs from od_task to fx_task */
ID mbx_od_to_fx = -1;

/* Shared crop buffer for ReID input (128 x 256 x 3 = 98304 bytes).
 * Placed in external 32MB PSRAM (.psram_bss) to save internal SRAM.
 * od_task writes the cropped person into this buffer, sets fx_busy = true,
 * then sends a mailbox message to fx_task.
 * Once fx_task completes nnlib_set_input(), fx_busy is cleared so od_task
 * can reuse this buffer for the next crop. */
__attribute__((section(".psram_bss"))) __attribute__((aligned(32)))
uint8_t reid_input_buf[128 * 256 * 3];

extern volatile bool fx_busy;
static uint8_t s_reid_rr_idx = 0;  /* Round-robin index cycling across detected persons */

/* Static AI context for OD model */
STAI_NETWORK_CONTEXT_DECLARE(od_network, STAI_OD_MODEL_CONTEXT_SIZE);

volatile bool npu_is_inferencing = false;
volatile uint32_t npu_inf_start_tick = 0;

/* Static mailbox message for passing crop job to fx_task */
static FxJobMsg_t s_fx_msg;

/* OD model configuration with vtable */
static nnlib_config_t od_nn_config = {
    .network = od_network,
    .external_weights_addr = (void*)0x71000000,
    .vtable = {
        .init        = stai_od_model_init,
        .run         = stai_od_model_run,
        .get_inputs  = stai_od_model_get_inputs,
        .get_outputs = stai_od_model_get_outputs,
    }
};

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

/* =========================================================================
 * NPU Watchdog Task (unchanged)
 * ========================================================================= */
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
    .itskpri = 9,
    .stksz = 4096,
    .task = npu_watchdog_task,
    .tskatr = TA_HLNG | TA_RNG0,
};

/* =========================================================================
 * OD Task — Object Detection (YOLOv8n)
 * ========================================================================= */
LOCAL void od_task(INT stacd, void *exinf) {
  (void)stacd;
  (void)exinf;

  PRINT("[OD] Starting OD Task...\r\n");

  /* --- Create RTOS Semaphores and Mailbox --- */

  /* 1. Camera frame semaphore (existing) */
  T_CSEM csem_frame = {.exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1};
  sem_od_frame_ready = tk_cre_sem(&csem_frame);
  if (sem_od_frame_ready <= 0) {
    PRINT("[OD ERROR] Failed to create OD semaphore\r\n");
    tk_ext_tsk();
    return;
  }

  /* 2. NPU mutex semaphore (NEW — shared with fx_task) */
  T_CSEM csem_npu = {.exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 1, .maxsem = 1};
  sem_npu = tk_cre_sem(&csem_npu);
  if (sem_npu <= 0) {
    PRINT("[OD ERROR] Failed to create NPU semaphore\r\n");
    tk_ext_tsk();
    return;
  }

  /* 3. Mailbox for OD -> FX job passing (NEW) */
  T_CMBX cmbx = {.exinf = NULL, .mbxatr = TA_TFIFO | TA_MFIFO};
  mbx_od_to_fx = tk_cre_mbx(&cmbx);
  if (mbx_od_to_fx <= 0) {
    PRINT("[OD ERROR] Failed to create OD->FX mailbox\r\n");
    tk_ext_tsk();
    return;
  }

  /* --- Initialize Hardware and OD Model --- */
  nnlib_hardware_init();        /* Clocks, flash, NPU IRQ — called once for all models */
  if (!nnlib_init(&od_nn_config)) {
    PRINT("[OD ERROR] NNLIB Init Failed\r\n");
    tk_ext_tsk();
    return;
  }
  PRINT("[OD] YOLO Model Initialized successfully!\r\n");

  /* --- Launch FX Task (after hardware and mailbox are ready) --- */
  start_fx_task();

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

    /* --- Acquire NPU, run YOLO --- */
    tk_wai_sem(sem_npu, 1, TMO_FEVR);

    if (!nnlib_set_input(&od_nn_config, ml_buffer, ML_WIDTH * ML_HEIGHT * 3)) {
        PRINT("[OD ERROR] Failed to set input buffer\r\n");
        tk_sig_sem(sem_npu, 1);
        continue;
    }

    uint32_t inf_ms = 0;
    npu_inf_start_tick = HAL_GetTick();
    npu_is_inferencing = true;
    bool inf_ret = nnlib_run_inference(&od_nn_config, &inf_ms);
    npu_is_inferencing = false;

    if (!inf_ret) {
        PRINT("[OD ERROR] Inference Failed or Hanged!\r\n");
        tk_sig_sem(sem_npu, 1);
        continue;
    }

    /* --- Get model output BEFORE releasing NPU mutex --- */
    float *bboxes = NULL;
    bool get_out_ok = nnlib_get_output(&od_nn_config, (void**)&bboxes);

    /* Release NPU mutex immediately after retrieving outputs so fx_task can run */
    tk_sig_sem(sem_npu, 1);

    inf_count++;

    /* --- Filter with NMS and Stream results --- */
    if (get_out_ok && bboxes != NULL) {
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

        /* Non-Maximum Suppression (IoU > 0.45 threshold, maintain stable spatial order) */
        DetectionBox_t detected_boxes[OD_MAX_STREAM_BOXES];
        uint8_t num_detected = 0;

        for (uint8_t i = 0; i < num_candidates; i++) {
            if (suppressed[i]) continue;

            for (uint8_t j = i + 1; j < num_candidates; j++) {
                if (suppressed[j]) continue;
                float iou = calculate_iou(
                    candidates[i].cx, candidates[i].cy, candidates[i].w, candidates[i].h,
                    candidates[j].cx, candidates[j].cy, candidates[j].w, candidates[j].h
                );
                if (iou > 0.45f) {
                    if (candidates[j].conf > candidates[i].conf) {
                        suppressed[i] = true;
                        break;
                    } else {
                        suppressed[j] = true;
                    }
                }
            }

            if (!suppressed[i] && num_detected < OD_MAX_STREAM_BOXES) {
                detected_boxes[num_detected++] = candidates[i];
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

            /* --- Decoupled ReID: process one person crop every 6 frames to keep NPU free for high-FPS YOLO --- */
            static uint32_t s_reid_throttle = 0;
            if (!fx_busy && (++s_reid_throttle % 6 == 0)) {
                uint8_t target_idx = s_reid_rr_idx % num_detected;

                image_crop_resize_bilinear(
                    ml_buffer, ML_WIDTH, ML_HEIGHT,
                    detected_boxes[target_idx].cx, detected_boxes[target_idx].cy,
                    detected_boxes[target_idx].w,  detected_boxes[target_idx].h,
                    reid_input_buf, 128, 256
                );
                /* Flush CPU D-Cache to ensure NPU DMA reads newly written crop pixels */
                SCB_CleanDCache_by_Addr((volatile void *)reid_input_buf, sizeof(reid_input_buf));

                static uint32_t s_crop_log_div = 0;
                if (++s_crop_log_div % 15 == 0) {
                    PRINT("[OD->FX] Crop box=%u/%u [cx=%d cy=%d w=%d h=%d] | frame=%lu\r\n",
                          (unsigned int)target_idx,
                          (unsigned int)num_detected,
                          (int)(detected_boxes[target_idx].cx * 1000.0f),
                          (int)(detected_boxes[target_idx].cy * 1000.0f),
                          (int)(detected_boxes[target_idx].w * 1000.0f),
                          (int)(detected_boxes[target_idx].h * 1000.0f),
                          (unsigned long)inf_count);
                }

                memset(&s_fx_msg.hdr, 0, sizeof(T_MSG));
                s_fx_msg.frame_id     = inf_count;
                s_fx_msg.num_detected = num_detected;
                s_fx_msg.box_index    = target_idx;
                s_fx_msg.top_box      = detected_boxes[target_idx];

                fx_busy = true;  /* Mark busy until fx_task sets input on NPU */
                tk_snd_mbx(mbx_od_to_fx, (T_MSG*)&s_fx_msg);

                s_reid_rr_idx++; /* Advance round-robin for next cycle */
            }

        } else if (inf_count % 10 == 0) {
            PRINT("--- [NO DETECTION] Max Conf: %d%% (Threshold: 30%%) | Infer: %lu ms\r\n",
                  (int)(max_conf * 100.0f), (unsigned long)inf_ms);
        }

        /* Update streamer with latest detection metadata (lightweight, asynchronous) */
        Ethernet_Streamer_UpdateDetections(
            inf_count, inf_ms, num_detected, detected_boxes
        );

        /* Yield CPU to allow same-priority Ethernet streamer task to transmit */
        tk_rot_rdq(0);
    }
  }
}

LOCAL ID tskid_od;
LOCAL T_CTSK ctsk_od = {
    .itskpri = 11,
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
