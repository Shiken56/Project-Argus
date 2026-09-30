#ifndef APP_IMAGE_UTILS_H
#define APP_IMAGE_UTILS_H

#include <stdint.h>

/**
 * @brief Nearest-neighbor crop+resize from a source RGB888 image to a
 *        destination RGB888 image.
 *
 * Extracts a bounding box region from the source image (specified in
 * normalized YOLO center-format coordinates) and resizes it to fill
 * the destination buffer using nearest-neighbor interpolation.
 *
 * @param src          Pointer to source image (RGB888, row-major)
 * @param src_w        Source image width in pixels
 * @param src_h        Source image height in pixels
 * @param cx_norm      Bounding box center X (0.0 to 1.0, normalized)
 * @param cy_norm      Bounding box center Y (0.0 to 1.0, normalized)
 * @param w_norm       Bounding box width    (0.0 to 1.0, normalized)
 * @param h_norm       Bounding box height   (0.0 to 1.0, normalized)
 * @param dst          Pointer to destination buffer (must be dst_w * dst_h * 3 bytes)
 * @param dst_w        Destination width  (e.g. 128 for OSNet)
 * @param dst_h        Destination height (e.g. 256 for OSNet)
 */
void image_crop_resize_nn(
    const uint8_t *src, int src_w, int src_h,
    float cx_norm, float cy_norm, float w_norm, float h_norm,
    uint8_t *dst, int dst_w, int dst_h
);

/**
 * @brief Bilinear interpolation crop+resize from a source RGB888 image to a
 *        destination RGB888 image.
 *
 * Provides smooth, anti-aliased scaling for ReID feature extraction.
 */
void image_crop_resize_bilinear(
    const uint8_t *src, int src_w, int src_h,
    float cx_norm, float cy_norm, float w_norm, float h_norm,
    uint8_t *dst, int dst_w, int dst_h
);

#endif /* APP_IMAGE_UTILS_H */
