#include "app_fx.h"
#include <string.h>
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "stm32n6xx_hal.h"
#include "nnlib.h"
#include "stai_fx_model.h"
#include "app_camera_ethernet_test.h"

#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

/* =========================================================================
 * External resources shared with od_task (created in app_od.c)
 * ========================================================================= */
extern ID sem_npu;                       /* NPU mutex semaphore (count=1) */
extern ID mbx_od_to_fx;                 /* Mailbox: OD -> FX job queue */
extern uint8_t reid_input_buf[128 * 256 * 3]; /* Single shared crop buffer */

/* Flag to protect reid_input_buf: true while waiting for/processing set_input, false once buffer is free */
volatile bool fx_busy = false;

/* =========================================================================
 * FX (ReID) Model Configuration
 * ========================================================================= */
STAI_NETWORK_CONTEXT_DECLARE(fx_network, STAI_FX_MODEL_CONTEXT_SIZE);

static nnlib_config_t fx_nn_config = {
    .network = fx_network,
    .external_weights_addr = (void*)0x71400000,  /* OSNet weights in flash (4MB past OD) */
    .vtable = {
        .init        = stai_fx_model_init,
        .run         = stai_fx_model_run,
        .get_inputs  = stai_fx_model_get_inputs,
        .get_outputs = stai_fx_model_get_outputs,
    }
};

/* =========================================================================
 * FX Task — Feature Extraction (OSNet ReID)
 * ========================================================================= */
LOCAL void fx_task(INT stacd, void *exinf) {
    (void)stacd;
    (void)exinf;

    PRINT("[FX] Feature Extraction Task started...\r\n");

    /* Initialize the FX model (hardware already initialized by od_task) */
    if (!nnlib_init(&fx_nn_config)) {
        PRINT("[FX ERROR] FX Model Init Failed!\r\n");
        tk_ext_tsk();
        return;
    }
    PRINT("[FX] OSNet Model Initialized successfully!\r\n");
    PRINT("[FX] Input: %u bytes | Output: %u bytes\r\n",
          (unsigned int)STAI_FX_MODEL_IN_1_SIZE_BYTES,
          (unsigned int)STAI_FX_MODEL_OUT_1_SIZE_BYTES);

    while (1) {
        /* Block until od_task sends a detection job */
        FxJobMsg_t *msg = NULL;
        ER err = tk_rcv_mbx(mbx_od_to_fx, (T_MSG**)&msg, TMO_FEVR);
        if (err != E_OK || msg == NULL) {
            continue;
        }

        /* --- Acquire NPU, run OSNet on shared reid_input_buf --- */
        tk_wai_sem(sem_npu, 1, TMO_FEVR);

        if (!nnlib_set_input(&fx_nn_config, reid_input_buf,
                             STAI_FX_MODEL_IN_1_SIZE_BYTES)) {
            PRINT("[FX ERROR] Failed to set FX input buffer\r\n");
            fx_busy = false;
            tk_sig_sem(sem_npu, 1);
            continue;
        }

        /* reid_input_buf is now DMA-loaded / consumed by NPU; safe for od_task to reuse */
        fx_busy = false;

        uint32_t inf_ms_reid = 0;
        bool inf_ok = nnlib_run_inference(&fx_nn_config, &inf_ms_reid);

        if (!inf_ok) {
            PRINT("[FX ERROR] OSNet Inference Failed!\r\n");
            tk_sig_sem(sem_npu, 1);
            continue;
        }

        /* --- Get embedding output BEFORE releasing NPU mutex --- */
        int8_t *emb_ptr = NULL;
        if (nnlib_get_output(&fx_nn_config, (void**)&emb_ptr) && emb_ptr != NULL) {
            /* Invalidate CPU D-Cache to ensure CPU reads fresh embedding bytes written by NPU */
            SCB_InvalidateDCache_by_Addr((volatile void *)emb_ptr, STAI_FX_MODEL_OUT_1_SIZE_BYTES);

            /* Convert model signed INT8 (zero-point -128) to clean unsigned UINT8 [0..255] */
            uint8_t u8_emb[STAI_FX_MODEL_OUT_1_SIZE_BYTES];
            for (uint16_t k = 0; k < STAI_FX_MODEL_OUT_1_SIZE_BYTES; k++) {
                u8_emb[k] = (uint8_t)((int)emb_ptr[k] + 128);
            }

            /* Send ReID embedding to Ethernet streamer, tagged with box coordinates and index */
            Ethernet_Streamer_UpdateReID(msg->frame_id, msg->box_index, &msg->top_box, (const int8_t*)u8_emb,
                                         STAI_FX_MODEL_OUT_1_SIZE_BYTES);

            PRINT("[FX] ReID done | frame=%lu | box=%u | ms=%lu\r\n[FX EMB 128]: [",
                  (unsigned long)msg->frame_id,
                  (unsigned int)msg->box_index,
                  (unsigned long)inf_ms_reid);
            for (uint16_t k = 0; k < STAI_FX_MODEL_OUT_1_SIZE_BYTES; k++) {
                PRINT("%u%s", (unsigned int)u8_emb[k], (k + 1 < STAI_FX_MODEL_OUT_1_SIZE_BYTES) ? ", " : "]\r\n");
            }
        }

        tk_sig_sem(sem_npu, 1);   /* Release NPU AFTER outputs are retrieved */

        tk_rot_rdq(0);  /* Yield to other same-priority tasks */
    }
}

/* =========================================================================
 * FX Task Creation
 * ========================================================================= */
LOCAL T_CTSK ctsk_fx = {
    .itskpri = 11,
    .stksz   = 8192,
    .task    = fx_task,
    .tskatr  = TA_HLNG | TA_RNG0,
};

void start_fx_task(void) {
    ID id = tk_cre_tsk(&ctsk_fx);
    if (id > 0) {
        tk_sta_tsk(id, 0);
        PRINT("[FX] Task created and started (Priority 11)\r\n");
    } else {
        PRINT("[FX ERROR] Failed to create FX task: %d\r\n", id);
    }
}
