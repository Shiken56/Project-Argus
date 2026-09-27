#!/usr/bin/env python3
"""
STM32N6 Real-Time Video + Object Detection Ethernet Visualizer
--------------------------------------------------------------
Receives synchronized 256x256 RGB888 camera frames and NPU
bounding box telemetry over UDP from STM32N6570-DK, and displays
an annotated live video stream in real-time.

Requirements:
    pip install opencv-python numpy
"""

import socket
import struct
import time
import sys
import numpy as np

try:
    import cv2
except ImportError:
    print("\r\n[ERROR] OpenCV is not installed!")
    print("Please install it by running: pip install opencv-python numpy\r\n")
    sys.exit(1)

# Protocol Constants
STREAM_MAGIC = 0x54524F4E  # "TRON" in ASCII
PKT_TYPE_VIDEO_CHUNK = 1
PKT_TYPE_OD_METADATA = 2

# Binary Formats (Little-Endian '<')
# VideoChunkHeader_t: magic(I), type(B), rsvd(B), chunk_idx(H), total_chunks(H), payload_len(H), frame_id(I) -> 16 bytes
VIDEO_HDR_FMT = '<IBBHHHI'
VIDEO_HDR_SIZE = struct.calcsize(VIDEO_HDR_FMT)

# DetectionBox_t: cx(f), cy(f), w(f), h(f), conf(f), class_id(H), rsvd(H) -> 24 bytes
BOX_FMT = '<fffffHH'
BOX_SIZE = struct.calcsize(BOX_FMT)

# OdMetadataPacket_t: magic(I), type(B), num_boxes(B), inf_ms(H), frame_id(I), w(H), h(H) -> 16 bytes
# Followed by 10 boxes (240 bytes) -> 256 bytes total
META_HDR_FMT = '<IBBHIHH'
META_HDR_SIZE = struct.calcsize(META_HDR_FMT)

LISTEN_IP = "0.0.0.0"
LISTEN_PORT = 5000

def main():
    print("=" * 60)
    print("  STM32N6 AI Vision Stream & Telemetry Visualizer  ")
    print("=" * 60)
    print(f"[*] Binding to UDP port {LISTEN_PORT} on all interfaces...")
    print(f"[*] Ensure your laptop Ethernet IP matches STM32 target (e.g. 192.168.1.100)")
    print("[*] Press 'q' or 'ESC' in the video window to quit.\r\n")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024) # 2MB socket buffer
    sock.bind((LISTEN_IP, LISTEN_PORT))

    # Frame reassembly cache: frame_id -> {chunk_idx: payload_bytes}
    frame_chunks = {}
    frame_meta = {}
    last_displayed_frame = 0

    fps_count = 0
    fps_start_time = time.time()
    current_fps = 0.0

    first_packet_received = False
    persistent_frame_buffer = bytearray(256 * 256 * 2) # Default to RGB565 (131,072 bytes)

    cv2.namedWindow("STM32N6 Object Detection Live Stream", cv2.WINDOW_AUTOSIZE)

    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except KeyboardInterrupt:
            break

        if not first_packet_received:
            print(f"[+] Successfully receiving streaming packets from {addr}!")
            first_packet_received = True

        if len(data) < 4:
            continue

        magic = struct.unpack('<I', data[:4])[0]
        if magic != STREAM_MAGIC:
            continue

        pkt_type = data[4]

        # ---------------------------------------------------------
        # 1. OD Metadata Packet (Bounding Boxes)
        # ---------------------------------------------------------
        if pkt_type == PKT_TYPE_OD_METADATA and len(data) >= META_HDR_SIZE:
            magic, p_type, num_boxes, inf_ms, frame_id, img_w, img_h = struct.unpack(
                META_HDR_FMT, data[:META_HDR_SIZE]
            )

            boxes = []
            offset = META_HDR_SIZE
            for i in range(min(num_boxes, 10)):
                if offset + BOX_SIZE <= len(data):
                    cx, cy, w, h, conf, class_id, _ = struct.unpack(BOX_FMT, data[offset:offset + BOX_SIZE])
                    boxes.append({
                        'cx': cx, 'cy': cy, 'w': w, 'h': h,
                        'conf': conf, 'class_id': class_id
                    })
                    offset += BOX_SIZE

            frame_meta[frame_id] = {
                'inf_ms': inf_ms,
                'boxes': boxes,
                'img_w': img_w,
                'img_h': img_h
            }

        # ---------------------------------------------------------
        # 2. Video Chunk Packet
        # ---------------------------------------------------------
        elif pkt_type == PKT_TYPE_VIDEO_CHUNK and len(data) >= VIDEO_HDR_SIZE:
            magic, p_type, rsvd, chunk_idx, total_chunks, payload_len, frame_id = struct.unpack(
                VIDEO_HDR_FMT, data[:VIDEO_HDR_SIZE]
            )

            payload = data[VIDEO_HDR_SIZE : VIDEO_HDR_SIZE + payload_len]

            # Auto-detect RGB565 (<= 100 chunks, 1400-byte MTU) vs RGB888 (> 100 chunks, 1024-byte MTU)
            is_rgb565 = (total_chunks <= 100)
            expected_bytes = 256 * 256 * 2 if is_rgb565 else 256 * 256 * 3
            chunk_stride = 1400 if is_rgb565 else 1024

            if len(persistent_frame_buffer) != expected_bytes:
                persistent_frame_buffer = bytearray(expected_bytes)

            if frame_id not in frame_chunks:
                frame_chunks[frame_id] = 0

            # Write chunk directly to persistent frame buffer (immune to minor packet loss)
            offset = chunk_idx * chunk_stride
            end_offset = offset + payload_len
            if end_offset <= len(persistent_frame_buffer):
                persistent_frame_buffer[offset:end_offset] = payload
                frame_chunks[frame_id] += 1

            # Render when frame completes or when the last chunk arrives
            if frame_chunks[frame_id] == total_chunks or chunk_idx == total_chunks - 1:
                # Convert raw buffer to NumPy OpenCV image (BGR)
                if is_rgb565:
                    img_565 = np.frombuffer(persistent_frame_buffer, dtype=np.uint8).reshape((256, 256, 2))
                    img_bgr = cv2.cvtColor(img_565, cv2.COLOR_BGR5652BGR)
                else:
                    img_rgb = np.frombuffer(persistent_frame_buffer, dtype=np.uint8).reshape((256, 256, 3))
                    img_bgr = cv2.cvtColor(img_rgb, cv2.COLOR_RGB2BGR)

                # Scale up to 512x512 for comfortable viewing on high-res monitors
                disp_w, disp_h = 512, 512
                img_disp = cv2.resize(img_bgr, (disp_w, disp_h), interpolation=cv2.INTER_NEAREST)

                # Lookup matching bounding box metadata
                meta = frame_meta.get(frame_id, None)
                inf_ms_text = "NPU: ~65 ms"
                box_count = 0

                if meta:
                    inf_ms_text = f"NPU: {meta['inf_ms']} ms"
                    box_count = len(meta['boxes'])

                    for box in meta['boxes']:
                        # Unpack normalized coordinates [0.0, 1.0] to display coordinates
                        cx = box['cx'] * disp_w
                        cy = box['cy'] * disp_h
                        bw = box['w'] * disp_w
                        bh = box['h'] * disp_h

                        x1 = int(max(0, cx - bw / 2.0))
                        y1 = int(max(0, cy - bh / 2.0))
                        x2 = int(min(disp_w - 1, cx + bw / 2.0))
                        y2 = int(min(disp_h - 1, cy + bh / 2.0))

                        conf_pct = int(box['conf'] * 100.0)

                        # Draw Neon Green Bounding Box
                        cv2.rectangle(img_disp, (x1, y1), (x2, y2), (0, 255, 0), 2)

                        # Label Header with Confidence
                        label = f"Obj: {conf_pct}%"
                        (label_w, label_h), _ = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.45, 1)
                        cv2.rectangle(img_disp, (x1, y1 - 18), (x1 + label_w + 6, y1), (0, 255, 0), -1)
                        cv2.putText(img_disp, label, (x1 + 3, y1 - 4),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 0), 1, cv2.LINE_AA)

                # Measure Display FPS
                fps_count += 1
                if time.time() - fps_start_time >= 1.0:
                    current_fps = fps_count / (time.time() - fps_start_time)
                    fps_count = 0
                    fps_start_time = time.time()

                # Overlay Status Banner at the top
                cv2.rectangle(img_disp, (0, 0), (disp_w, 28), (20, 20, 20), -1)
                status_text = f"Frame #{frame_id} | {inf_ms_text} | Stream: {current_fps:.1f} FPS | Detections: {box_count}"
                cv2.putText(img_disp, status_text, (8, 19),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 255), 1, cv2.LINE_AA)

                # Show on Screen
                cv2.imshow("STM32N6 Object Detection Live Stream", img_disp)
                last_displayed_frame = frame_id

                key = cv2.waitKey(1) & 0xFF
                if key == ord('q') or key == 27:
                    break

                # Cleanup older frames to keep memory tiny
                cutoff = frame_id - 5
                old_frames = [k for k in frame_chunks.keys() if k < cutoff]
                for k in old_frames:
                    del frame_chunks[k]
                old_metas = [k for k in frame_meta.keys() if k < cutoff]
                for k in old_metas:
                    del frame_meta[k]

    sock.close()
    cv2.destroyAllWindows()
    print("[*] Stream closed.")

if __name__ == '__main__':
    main()
