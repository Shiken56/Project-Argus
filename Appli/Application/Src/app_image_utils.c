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
        memset(dst, 0, (uint32_t)(dst_w * dst_h * 3));
        return;
    }

    /* Convert normalized center-format to pixel dimensions */
    float box_w = w_norm * (float)src_w;
    float box_h = h_norm * (float)src_h;
    float cx = cx_norm * (float)src_w;
    float cy = cy_norm * (float)src_h;

    if (box_w <= 1.0f || box_h <= 1.0f) {
        memset(dst, 0, (uint32_t)(dst_w * dst_h * 3));
        return;
    }

    /* Preserve aspect ratio (expand to match dst_w / dst_h) */
    float target_ratio = (float)dst_w / (float)dst_h;
    float box_ratio = box_w / box_h;

    float crop_w = box_w;
    float crop_h = box_h;

    if (box_ratio < target_ratio) {
        /* Box is narrower/taller than target: expand width */
        crop_w = box_h * target_ratio;
    } else {
        /* Box is wider than target: expand height */
        crop_h = box_w / target_ratio;
    }

    float x1 = cx - crop_w * 0.5f;
    float x2 = cx + crop_w * 0.5f;
    float y1 = cy - crop_h * 0.5f;
    float y2 = cy + crop_h * 0.5f;

    /* Shift crop box to stay within image boundaries if possible */
    if (crop_w <= (float)src_w) {
        if (x1 < 0.0f) {
            x2 += -x1;
            x1 = 0.0f;
        } else if (x2 > (float)src_w) {
            x1 -= (x2 - (float)src_w);
            x2 = (float)src_w;
        }
    }

    if (crop_h <= (float)src_h) {
        if (y1 < 0.0f) {
            y2 += -y1;
            y1 = 0.0f;
        } else if (y2 > (float)src_h) {
            y1 -= (y2 - (float)src_h);
            y2 = (float)src_h;
        }
    }

    int step_x_fp = (int)((crop_w / (float)dst_w) * 256.0f);
    int step_y_fp = (int)((crop_h / (float)dst_h) * 256.0f);
    int start_x_fp = (int)(x1 * 256.0f);
    int start_y_fp = (int)(y1 * 256.0f);

    /* Nearest-neighbor resize from aspect-preserved crop region to dst */
    for (int dy = 0; dy < dst_h; dy++) {
        int src_y_fp = start_y_fp + dy * step_y_fp;
        int sy = clamp_i(src_y_fp >> 8, 0, src_h - 1);
        const uint8_t *src_row = &src[sy * src_w * 3];
        uint8_t *dst_row = &dst[dy * dst_w * 3];

        for (int dx = 0; dx < dst_w; dx++) {
            int src_x_fp = start_x_fp + dx * step_x_fp;
            int sx = clamp_i(src_x_fp >> 8, 0, src_w - 1);

            const uint8_t *src_px = &src_row[sx * 3];
            uint8_t *dst_px = &dst_row[dx * 3];
            dst_px[0] = src_px[0];
            dst_px[1] = src_px[1];
            dst_px[2] = src_px[2];
        }
    }
}

void image_crop_resize_bilinear(
    const uint8_t *src, int src_w, int src_h,
    float cx_norm, float cy_norm, float w_norm, float h_norm,
    uint8_t *dst, int dst_w, int dst_h)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        memset(dst, 0, (uint32_t)(dst_w * dst_h * 3));
        return;
    }

    /* Convert normalized center-format to pixel dimensions */
    float box_w = w_norm * (float)src_w;
    float box_h = h_norm * (float)src_h;
    float cx = cx_norm * (float)src_w;
    float cy = cy_norm * (float)src_h;

    if (box_w <= 1.0f || box_h <= 1.0f) {
        memset(dst, 0, (uint32_t)(dst_w * dst_h * 3));
        return;
    }

    /* Preserve aspect ratio (expand to match dst_w / dst_h, e.g. 128 / 256 = 0.5) */
    float target_ratio = (float)dst_w / (float)dst_h;
    float box_ratio = box_w / box_h;

    float crop_w = box_w;
    float crop_h = box_h;

    if (box_ratio < target_ratio) {
        /* Box is narrower/taller than target: expand width */
        crop_w = box_h * target_ratio;
    } else {
        /* Box is wider than target: expand height */
        crop_h = box_w / target_ratio;
    }

    float x1 = cx - crop_w * 0.5f;
    float x2 = cx + crop_w * 0.5f;
    float y1 = cy - crop_h * 0.5f;
    float y2 = cy + crop_h * 0.5f;

    /* Shift crop box to stay within image boundaries if possible */
    if (crop_w <= (float)src_w) {
        if (x1 < 0.0f) {
            x2 += -x1;
            x1 = 0.0f;
        } else if (x2 > (float)src_w) {
            x1 -= (x2 - (float)src_w);
            x2 = (float)src_w;
        }
    }

    if (crop_h <= (float)src_h) {
        if (y1 < 0.0f) {
            y2 += -y1;
            y1 = 0.0f;
        } else if (y2 > (float)src_h) {
            y1 -= (y2 - (float)src_h);
            y2 = (float)src_h;
        }
    }

    /* Fixed-point scale factors (8-bit fractional precision: 1.0 = 256) */
    int step_x_fp = (int)((crop_w / (float)dst_w) * 256.0f);
    int step_y_fp = (int)((crop_h / (float)dst_h) * 256.0f);
    int start_x_fp = (int)(x1 * 256.0f);
    int start_y_fp = (int)(y1 * 256.0f);

    for (int dy = 0; dy < dst_h; dy++) {
        int src_y_fp = start_y_fp + dy * step_y_fp;
        int y0 = src_y_fp >> 8;
        int fy = src_y_fp & 0xFF;  /* fractional y in [0, 255] */
        int y1_idx = y0 + 1;

        int cy0 = clamp_i(y0, 0, src_h - 1);
        int cy1 = clamp_i(y1_idx, 0, src_h - 1);

        int row0_offset = cy0 * src_w * 3;
        int row1_offset = cy1 * src_w * 3;
        uint8_t *dst_row = &dst[dy * dst_w * 3];

        for (int dx = 0; dx < dst_w; dx++) {
            int src_x_fp = start_x_fp + dx * step_x_fp;
            int x0 = src_x_fp >> 8;
            int fx = src_x_fp & 0xFF;  /* fractional x in [0, 255] */
            int x1_idx = x0 + 1;

            int cx0 = clamp_i(x0, 0, src_w - 1);
            int cx1 = clamp_i(x1_idx, 0, src_w - 1);

            const uint8_t *p00 = &src[row0_offset + cx0 * 3];
            const uint8_t *p01 = &src[row0_offset + cx1 * 3];
            const uint8_t *p10 = &src[row1_offset + cx0 * 3];
            const uint8_t *p11 = &src[row1_offset + cx1 * 3];

            int w00 = (256 - fx) * (256 - fy);
            int w01 = fx * (256 - fy);
            int w10 = (256 - fx) * fy;
            int w11 = fx * fy;

            uint8_t *dst_px = &dst_row[dx * 3];
            dst_px[0] = (uint8_t)((p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11 + 32768) >> 16);
            dst_px[1] = (uint8_t)((p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11 + 32768) >> 16);
            dst_px[2] = (uint8_t)((p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11 + 32768) >> 16);
        }
    }
}

