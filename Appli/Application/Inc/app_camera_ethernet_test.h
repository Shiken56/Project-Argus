#ifndef APP_CAMERA_ETHERNET_TEST_H
#define APP_CAMERA_ETHERNET_TEST_H

#include <stdint.h>
#include <stdbool.h>

#define OD_MAX_STREAM_BOXES 10
#define STREAM_MAGIC 0x54524F4E /* "TRON" in ASCII */

#define PKT_TYPE_VIDEO_CHUNK 1
#define PKT_TYPE_OD_METADATA 2

#pragma pack(push, 1)
typedef struct {
    float cx;               /* Center X (0.0 to 1.0) */
    float cy;               /* Center Y (0.0 to 1.0) */
    float w;                /* Width    (0.0 to 1.0) */
    float h;                /* Height   (0.0 to 1.0) */
    float conf;             /* Confidence score (0.0 to 1.0) */
    uint16_t class_id;      /* Object Class */
    uint16_t reserved;
} DetectionBox_t;

typedef struct {
    uint32_t magic;         /* STREAM_MAGIC (0x54524F4E) */
    uint8_t  pkt_type;      /* PKT_TYPE_OD_METADATA (2) */
    uint8_t  num_boxes;     /* Number of detected boxes (0 to OD_MAX_STREAM_BOXES) */
    uint16_t inference_ms;  /* Model inference time in milliseconds */
    uint32_t frame_id;      /* Synchronized with video frame_id */
    uint16_t img_width;     /* Image Width  (256) */
    uint16_t img_height;    /* Image Height (256) */
    DetectionBox_t boxes[OD_MAX_STREAM_BOXES];
} OdMetadataPacket_t;

typedef struct {
    uint32_t magic;         /* STREAM_MAGIC (0x54524F4E) */
    uint8_t  pkt_type;      /* PKT_TYPE_VIDEO_CHUNK (1) */
    uint8_t  reserved;
    uint16_t chunk_idx;     /* Current chunk (0, 1, ...) */
    uint16_t total_chunks;  /* Total chunks for this frame */
    uint16_t payload_len;   /* Length of byte array following header */
    uint32_t frame_id;      /* Synchronized with OdMetadataPacket_t */
} VideoChunkHeader_t;
#pragma pack(pop)

/* Initialize the UDP streamer connection and background RTOS task */
void Ethernet_Streamer_Init(void);

/* Send a video frame directly from camera task (Decoupled, 20-30 FPS) */
void Ethernet_Streamer_SendVideoFrame(
    const uint8_t *frame_buffer,
    uint32_t width,
    uint32_t height,
    uint8_t bpp,
    uint32_t frame_id
);

/* Update latest detection bounding boxes from OD task (Decoupled, 10-14 FPS) */
void Ethernet_Streamer_UpdateDetections(
    uint32_t frame_id,
    uint32_t inference_ms,
    uint8_t num_boxes,
    const DetectionBox_t *boxes
);

/* Backward compatibility wrapper */
void Ethernet_Streamer_SendFrameWithDetections(
    const uint8_t *frame_buffer,
    uint32_t width,
    uint32_t height,
    uint8_t bpp,
    uint32_t frame_id,
    uint32_t inference_ms,
    uint8_t num_boxes,
    const DetectionBox_t *boxes
);

#endif // APP_CAMERA_ETHERNET_TEST_H
