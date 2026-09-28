#include "app_image_utils.h"
#include <string.h>

/**
 * @brief Clamp an integer value to [min_val, max_val].
 */
static inline int clamp_i(int val, int min_val, int max_val) {
    if (val < min_val) return min_val;
    if (val > max_val) return max_val;
    return val;
}

void image_crop_resize_nn(
    const uint8_t *src, int src_w, int src_h,
    float cx_norm, float cy_norm, float w_norm, float h_norm,
    uint8_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        /* Fill black on invalid input */
        memset(dst, 0, (uint32_t)(dst_w * dst_h * 3));
        return;
    }

    /* Convert normalized center-format to pixel coordinates */
    float box_x1 = (cx_norm - w_norm * 0.5f) * (float)src_w;
    float box_y1 = (cy_norm - h_norm * 0.5f) * (float)src_h;
    float box_x2 = (cx_norm + w_norm * 0.5f) * (float)src_w;
    float box_y2 = (cy_norm + h_norm * 0.5f) * (float)src_h;

    /* Clamp to image boundaries */
    int crop_x1 = clamp_i((int)box_x1, 0, src_w - 1);
    int crop_y1 = clamp_i((int)box_y1, 0, src_h - 1);
    int crop_x2 = clamp_i((int)box_x2, 1, src_w);
    int crop_y2 = clamp_i((int)box_y2, 1, src_h);

    int crop_w = crop_x2 - crop_x1;
    int crop_h = crop_y2 - crop_y1;

    if (crop_w <= 0 || crop_h <= 0) {
        memset(dst, 0, (uint32_t)(dst_w * dst_h * 3));
        return;
    }

    /* Nearest-neighbor resize from crop region to dst */
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = crop_y1 + (dy * crop_h) / dst_h;
        if (sy >= src_h) sy = src_h - 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx = crop_x1 + (dx * crop_w) / dst_w;
            if (sx >= src_w) sx = src_w - 1;

            const uint8_t *src_px = &src[(sy * src_w + sx) * 3];
            uint8_t *dst_px = &dst[(dy * dst_w + dx) * 3];
            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
        }
    }
}
