#ifndef APP_FX_H
#define APP_FX_H

#include <tk/tkernel.h>
#include "app_camera_ethernet_test.h"

/**
 * @brief Message passed from od_task to fx_task via TRON mailbox.
 *        T_MSG MUST be the first field — this is a hard TRON mailbox protocol requirement.
 */
typedef struct {
    T_MSG           hdr;            /* MUST be first field — TRON mailbox protocol */
    uint32_t        frame_id;       /* Which inference frame this came from */
    uint8_t         num_detected;   /* Number of boxes detected by OD */
    DetectionBox_t  top_box;        /* The best (highest-confidence) detection box */
} FxJobMsg_t;

/**
 * @brief Launch the Feature Extraction (ReID) task.
 *        Must be called AFTER nnlib_hardware_init() has been called by od_task.
 */
void start_fx_task(void);

#endif /* APP_FX_H */
