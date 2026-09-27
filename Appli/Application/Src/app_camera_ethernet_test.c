#include "app_camera_ethernet_test.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include <string.h>

#undef _B
#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#define PRINT(fmt, ...) tm_printf((const UB *)(fmt), ##__VA_ARGS__)

/* ========================================================================= */
/* CONFIGURATION: LAPTOP / PC ETHERNET DESTINATION                           */
/* ========================================================================= */
#define PC_IP_1 192
#define PC_IP_2 168
#define PC_IP_3 1
#define PC_IP_4 255  /* Subnet broadcast (192.168.1.255) - bypasses ARP resolution so UDP is sent immediately */

#define TARGET_PORT 5000
#define UDP_CHUNK_PAYLOAD_SIZE 1400

static struct udp_pcb *stream_pcb = NULL;
static ip_addr_t target_ip;
static ID sem_stream_ready = 0;

/* Double Buffers in PSRAM for 256x256 RGB565 streaming (avoids internal SRAM collision with 0x34100000) */
static __attribute__((aligned(32))) __attribute__((section(".psram_bss"))) uint16_t s_rgb565_stream_buf[2][256 * 256];
static volatile uint8_t s_active_read_buf = 0;
static volatile bool s_stream_busy = false;
static uint32_t s_frame_bytes[2] = {0, 0};
static uint32_t s_stream_frame_id[2] = {0, 0};

/* Shared detection metadata (updated by OD task, transmitted by Streamer task) */
static OdMetadataPacket_t s_meta_pkt;

static inline void convert_rgb888_to_rgb565(const uint8_t *src, uint16_t *dst, uint32_t num_pixels) {
    for (uint32_t i = 0; i < num_pixels; i++) {
        uint8_t r = src[i * 3 + 0];
        uint8_t g = src[i * 3 + 1];
        uint8_t b = src[i * 3 + 2];
        dst[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
}

/* Dedicated Background RTOS Ethernet Streaming Task (Priority 11) */
static void ethernet_stream_task(INT stacd, void *exinf) {
    (void)stacd;
    (void)exinf;

    PRINT("[ETH STREAM] Decoupled Streamer Task started (Priority 11)...\r\n");

    while (1) {
        /* Sleep until camera task signals a new frame */
        ER err = tk_wai_sem(sem_stream_ready, 1, TMO_FEVR);
        if (err != E_OK) continue;

        if (stream_pcb == NULL) {
            s_stream_busy = false;
            continue;
        }

        uint8_t r_idx = s_active_read_buf;
        const uint8_t *frame_data = (const uint8_t *)s_rgb565_stream_buf[r_idx];
        uint32_t total_bytes = s_frame_bytes[r_idx];
        uint32_t frame_id = s_stream_frame_id[r_idx];

        /* 1. Transmit Metadata Packet with current detection state */
        struct pbuf *p_meta = pbuf_alloc(PBUF_TRANSPORT, sizeof(OdMetadataPacket_t), PBUF_RAM);
        if (p_meta != NULL) {
            OdMetadataPacket_t meta_copy;
            memcpy(&meta_copy, &s_meta_pkt, sizeof(OdMetadataPacket_t));
            meta_copy.frame_id = frame_id;
            memcpy(p_meta->payload, &meta_copy, sizeof(OdMetadataPacket_t));
            udp_sendto(stream_pcb, p_meta, &target_ip, TARGET_PORT);
            pbuf_free(p_meta);
        }

        /* 2. Transmit Video Chunks */
        uint16_t total_chunks = (total_bytes + UDP_CHUNK_PAYLOAD_SIZE - 1) / UDP_CHUNK_PAYLOAD_SIZE;

        for (uint16_t c = 0; c < total_chunks; c++) {
            uint32_t offset = c * UDP_CHUNK_PAYLOAD_SIZE;
            uint16_t chunk_len = UDP_CHUNK_PAYLOAD_SIZE;
            if (offset + chunk_len > total_bytes) {
                chunk_len = total_bytes - offset;
            }

            uint16_t packet_size = sizeof(VideoChunkHeader_t) + chunk_len;
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, packet_size, PBUF_RAM);
            if (p != NULL) {
                VideoChunkHeader_t *hdr = (VideoChunkHeader_t *)p->payload;
                hdr->magic = STREAM_MAGIC;
                hdr->pkt_type = PKT_TYPE_VIDEO_CHUNK;
                hdr->reserved = 0;
                hdr->chunk_idx = c;
                hdr->total_chunks = total_chunks;
                hdr->payload_len = chunk_len;
                hdr->frame_id = frame_id;

                memcpy((uint8_t *)p->payload + sizeof(VideoChunkHeader_t), frame_data + offset, chunk_len);

                udp_sendto(stream_pcb, p, &target_ip, TARGET_PORT);
                pbuf_free(p);

                /* Cooperative yield every 16 chunks so OD task is never locked out */
                if ((c & 15) == 0) {
                    tk_rot_rdq(0);
                }
            }
        }

        s_stream_busy = false;
        tk_rot_rdq(0); /* Yield to other tasks */
    }
}

void Ethernet_Streamer_Init(void) {
    if (stream_pcb == NULL) {
        memset(&s_meta_pkt, 0, sizeof(s_meta_pkt));
        s_meta_pkt.magic = STREAM_MAGIC;
        s_meta_pkt.pkt_type = PKT_TYPE_OD_METADATA;

        stream_pcb = udp_new();
        if (stream_pcb == NULL) {
            PRINT("[ETH STREAM ERROR] udp_new() failed!\r\n");
            return;
        }

        IP4_ADDR(&target_ip, PC_IP_1, PC_IP_2, PC_IP_3, PC_IP_4);
        udp_bind(stream_pcb, IP_ADDR_ANY, 0);

        /* Create TRON Semaphore for Triggering */
        T_CSEM csem = {.exinf = NULL, .sematr = TA_TFIFO, .isemcnt = 0, .maxsem = 1};
        sem_stream_ready = tk_cre_sem(&csem);

/* Decimation factor: 2 = send 15 FPS (smooth display, balanced for NPU bus), 1 = 30 FPS */
#define STREAM_DECIMATION 2

        /* Create Dedicated Background Task (Priority 11 - co-operates with OD task) */
        T_CTSK ctsk = {
            .exinf = NULL,
            .tskatr = TA_HLNG | TA_RNG0,
            .task = ethernet_stream_task,
            .itskpri = 11,
            .stksz = 4096
        };
        ID eth_tsk = tk_cre_tsk(&ctsk);
        if (eth_tsk > 0) {
            tk_sta_tsk(eth_tsk, 0);
            PRINT("[ETH STREAM] Initialized UDP target: %d.%d.%d.%d:%d (Priority 11, Balanced)\r\n",
                  PC_IP_1, PC_IP_2, PC_IP_3, PC_IP_4, TARGET_PORT);
        } else {
            PRINT("[ETH STREAM ERROR] Failed to create streamer task: %d\r\n", eth_tsk);
        }
    }
}

/* Called directly from Camera Task on every frame (30 FPS) */
void Ethernet_Streamer_SendVideoFrame(
    const uint8_t *frame_buffer,
    uint32_t width,
    uint32_t height,
    uint8_t bpp,
    uint32_t frame_id
) {
    if (sem_stream_ready <= 0) return;

    /* Rate decimation: Send at 15 FPS to leave bus bandwidth for NPU */
    static uint32_t s_stream_div = 0;
    if (++s_stream_div % STREAM_DECIMATION != 0) {
        return;
    }

    /* If previous frame transmission is still in progress, drop this frame (zero lag) */
    if (s_stream_busy) {
        return;
    }

    uint8_t write_idx = s_active_read_buf ^ 1;

    if (bpp == 3) {
        /* Ultra-fast RGB888 -> RGB565 conversion directly into internal cached SRAM */
        convert_rgb888_to_rgb565(frame_buffer, s_rgb565_stream_buf[write_idx], width * height);
        s_frame_bytes[write_idx] = width * height * 2;
    } else {
        memcpy(s_rgb565_stream_buf[write_idx], frame_buffer, width * height * bpp);
        s_frame_bytes[write_idx] = width * height * bpp;
    }

    s_stream_frame_id[write_idx] = frame_id;
    s_active_read_buf = write_idx;
    s_stream_busy = true;

    /* Wake up Ethernet Streamer Task */
    tk_sig_sem(sem_stream_ready, 1);
}

/* Called from OD Task whenever an inference completes (~10-14 FPS) */
void Ethernet_Streamer_UpdateDetections(
    uint32_t frame_id,
    uint32_t inference_ms,
    uint8_t num_boxes,
    const DetectionBox_t *boxes
) {
    s_meta_pkt.magic = STREAM_MAGIC;
    s_meta_pkt.pkt_type = PKT_TYPE_OD_METADATA;
    s_meta_pkt.num_boxes = (num_boxes > OD_MAX_STREAM_BOXES) ? OD_MAX_STREAM_BOXES : num_boxes;
    s_meta_pkt.inference_ms = (uint16_t)inference_ms;
    s_meta_pkt.frame_id = frame_id;
    s_meta_pkt.img_width = 256;
    s_meta_pkt.img_height = 256;

    for (uint8_t i = 0; i < s_meta_pkt.num_boxes; i++) {
        s_meta_pkt.boxes[i] = boxes[i];
    }
}

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
) {
    Ethernet_Streamer_UpdateDetections(frame_id, inference_ms, num_boxes, boxes);
    Ethernet_Streamer_SendVideoFrame(frame_buffer, width, height, bpp, frame_id);
}
