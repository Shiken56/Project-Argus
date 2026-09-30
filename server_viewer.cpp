#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define STREAM_MAGIC 0x54524F4EU
#define PKT_TYPE_VIDEO_CHUNK   0x01
#define PKT_TYPE_OD_METADATA   0x02
#define PKT_TYPE_REID_METADATA 0x03

#define LISTEN_PORT 5000
#define OD_MAX_BOXES 10
#define REID_EMBEDDING_DIM 128
#define REID_SIMILARITY_THRESHOLD 0.91f  // 91% threshold: clean separation for same person (>91%) vs others (~82%)
#define REID_CONFIRMATION_COUNT   3      // Require 3 matching observations before confirming new person

#pragma pack(push, 1)
struct DetectionBox_t {
    float cx;
    float cy;
    float w;
    float h;
    float conf;
    uint16_t class_id;
    uint16_t reserved;
};

struct OdMetadataPacket_t {
    uint32_t magic;
    uint8_t  pkt_type;
    uint8_t  num_boxes;
    uint16_t inference_ms;
    uint32_t frame_id;
    uint16_t img_width;
    uint16_t img_height;
    DetectionBox_t boxes[OD_MAX_BOXES];
};

struct VideoChunkHeader_t {
    uint32_t magic;
    uint8_t  pkt_type;
    uint8_t  reserved;
    uint16_t chunk_idx;
    uint16_t total_chunks;
    uint16_t payload_len;
    uint32_t frame_id;
};

struct ReidMetadataPacket_t {
    uint32_t       magic;          // STREAM_MAGIC (0x54524F4E)
    uint8_t        pkt_type;       // PKT_TYPE_REID_METADATA (3)
    uint8_t        box_index;      // Detection index
    uint16_t       embedding_len;  // 128
    uint32_t       frame_id;       // Synchronized with video frame_id
    DetectionBox_t box;            // Bounding box for this crop
    int8_t         embedding[REID_EMBEDDING_DIM]; // Signed INT8 OSNet embedding vector
};
#pragma pack(pop)

// UI Dimensions
static const int FRAME_W = 256;
static const int FRAME_H = 256;
static const int DISP_W = 512;
static const int DISP_H = 512;
static const int HEADER_H = 36;
static const int SIDEBAR_W = 320;
static const int TOTAL_W = DISP_W + SIDEBAR_W;
static const int TOTAL_H = DISP_H + HEADER_H;

// Distinct Neon/Vivid colors for identities
static const COLORREF ID_COLORS[] = {
    RGB(0, 255, 255),    // Person 1: Cyan
    RGB(255, 140, 0),    // Person 2: Vivid Orange
    RGB(50, 255, 120),   // Person 3: Neon Green
    RGB(255, 60, 200),   // Person 4: Vibrant Magenta
    RGB(255, 230, 20),   // Person 5: Yellow
    RGB(130, 110, 255),  // Person 6: Purple
    RGB(0, 190, 255),    // Person 7: Sky Blue
    RGB(255, 80, 80)     // Person 8: Coral Red
};
static const int NUM_ID_COLORS = sizeof(ID_COLORS) / sizeof(ID_COLORS[0]);

// ReID Profile Structure
struct PersonProfile {
    int id;
    std::vector<float> feature;              // L2 normalized representative feature vector
    std::vector<std::vector<float>> history; // Rolling history of observed embeddings
    COLORREF color;
    uint32_t last_seen_frame;
    int match_count;
    float last_similarity;
    std::chrono::steady_clock::time_point last_seen_time;
};

// Spatial Multi-Object Track Structure (mirrors ST's TrackObject)
struct ActiveTrack {
    int track_id;               // Sequential Track ID (1, 2, ...)
    int person_id;              // Confirmed Gallery Person ID (1, 2, ...) or 0 if unassigned
    float sim;                  // Last match similarity
    COLORREF color;             // Color associated with person or neutral
    DetectionBox_t box;         // Last known bounding box coordinates (cx, cy, w, h)
    std::vector<float> feature; // Feature template for this track
    bool has_feature;
    int observation_count;      // Consecutive detections
    std::chrono::steady_clock::time_point last_seen;
};
static std::vector<ActiveTrack> g_tracks;
static int g_next_track_id = 1;

// Thread Synchronization & Shared Data
static CRITICAL_SECTION g_cs;
static std::vector<uint32_t> g_rgb32_buffer(FRAME_W * FRAME_H, 0); // 32-bit XRGB
static OdMetadataPacket_t g_latest_meta = {};
static uint32_t g_current_frame_id = 0;
static double g_stream_fps = 0.0;
static bool g_has_frame = false;
static bool g_running = true;

// ReID gallery & live raw embedding
static std::vector<PersonProfile> g_gallery;
static std::vector<int8_t> g_raw_latest_embedding(REID_EMBEDDING_DIM, 0);
static uint32_t g_latest_reid_box = 0;
static uint32_t g_latest_reid_frame = 0;
static uint32_t g_reid_total_count = 0;

// Window Handle
static HWND g_hwnd = NULL;

// Helper: Normalize clean UINT8 [0..255] vector directly (as emitted by firmware)
static std::vector<float> normalize_int8_embedding(const int8_t* raw, size_t len) {
    if (len == 0 || len > REID_EMBEDDING_DIM) len = REID_EMBEDDING_DIM;
    const uint8_t* u8_raw = reinterpret_cast<const uint8_t*>(raw);
    std::vector<float> vec(len);
    float sum_sq = 0.0f;
    for (size_t i = 0; i < len; i++) {
        float val = static_cast<float>(u8_raw[i]);
        vec[i] = val;
        sum_sq += val * val;
    }
    float norm = std::sqrt(sum_sq);
    if (norm > 1e-6f) {
        for (size_t i = 0; i < len; i++) {
            vec[i] /= norm;
        }
    }
    return vec;
}

// Helper: Compute Cosine Similarity between two L2-normalized float vectors
static float compute_cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f;
    for (size_t i = 0; i < a.size(); i++) {
        dot += a[i] * b[i];
    }
    return dot;
}

// Helper: Compute Intersection Over Union (IoU) between two detection boxes
static float compute_iou(const DetectionBox_t& a, const DetectionBox_t& b) {
    float a_x1 = a.cx - a.w * 0.5f;
    float a_y1 = a.cy - a.h * 0.5f;
    float a_x2 = a.cx + a.w * 0.5f;
    float a_y2 = a.cy + a.h * 0.5f;

    float b_x1 = b.cx - b.w * 0.5f;
    float b_y1 = b.cy - b.h * 0.5f;
    float b_x2 = b.cx + b.w * 0.5f;
    float b_y2 = b.cy + b.h * 0.5f;

    float inter_x1 = (std::max)(a_x1, b_x1);
    float inter_y1 = (std::max)(a_y1, b_y1);
    float inter_x2 = (std::min)(a_x2, b_x2);
    float inter_y2 = (std::min)(a_y2, b_y2);

    float inter_w = (std::max)(0.0f, inter_x2 - inter_x1);
    float inter_h = (std::max)(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;

    float a_area = a.w * a.h;
    float b_area = b.w * b.h;
    float union_area = a_area + b_area - inter_area;

    if (union_area <= 1e-6f) return 0.0f;
    return inter_area / union_area;
}

// Cross-Identity Deduplication: Compare all gallery profiles and merge duplicate identities
static void reconcile_and_merge_gallery(std::chrono::steady_clock::time_point now_t) {
    if (g_gallery.size() < 2) return;

    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < g_gallery.size() && !merged; i++) {
            for (size_t j = i + 1; j < g_gallery.size() && !merged; j++) {
                // 1. Mean template similarity
                float sim_mean = compute_cosine_similarity(g_gallery[i].feature, g_gallery[j].feature);

                // 2. Cross history maximum similarity
                float max_cross_sim = sim_mean;
                for (const auto& f_i : g_gallery[i].history) {
                    for (const auto& f_j : g_gallery[j].history) {
                        float s = compute_cosine_similarity(f_i, f_j);
                        if (s > max_cross_sim) max_cross_sim = s;
                    }
                }

                // If cross similarity is high (>= 0.88f), they belong to the same person!
                if (sim_mean >= 0.88f || max_cross_sim >= 0.90f) {
                    int keep_idx = (int)i;
                    int drop_idx = (int)j;

                    // Prefer the identity with more observations/matches
                    if (g_gallery[drop_idx].match_count > g_gallery[keep_idx].match_count) {
                        std::swap(keep_idx, drop_idx);
                    }

                    int keep_id = g_gallery[keep_idx].id;
                    int drop_id = g_gallery[drop_idx].id;

                    // Merge feature via weighted average
                    float w_keep = (float)std::max(1, g_gallery[keep_idx].match_count);
                    float w_drop = (float)std::max(1, g_gallery[drop_idx].match_count);
                    float total_w = w_keep + w_drop;

                    for (size_t k = 0; k < g_gallery[keep_idx].feature.size(); k++) {
                        g_gallery[keep_idx].feature[k] = (w_keep * g_gallery[keep_idx].feature[k] + w_drop * g_gallery[drop_idx].feature[k]) / total_w;
                    }

                    // Re-normalize merged feature
                    float sum_sq = 0.0f;
                    for (float v : g_gallery[keep_idx].feature) sum_sq += v * v;
                    float n_val = std::sqrt(sum_sq);
                    if (n_val > 1e-6f) {
                        for (float &v : g_gallery[keep_idx].feature) v /= n_val;
                    }

                    // Merge histories (keep up to 10 latest)
                    for (const auto& h : g_gallery[drop_idx].history) {
                        g_gallery[keep_idx].history.push_back(h);
                    }
                    if (g_gallery[keep_idx].history.size() > 10) {
                        g_gallery[keep_idx].history.erase(g_gallery[keep_idx].history.begin(),
                                                          g_gallery[keep_idx].history.begin() + (g_gallery[keep_idx].history.size() - 10));
                    }

                    g_gallery[keep_idx].match_count += g_gallery[drop_idx].match_count;
                    if (g_gallery[drop_idx].last_seen_time > g_gallery[keep_idx].last_seen_time) {
                        g_gallery[keep_idx].last_seen_time = g_gallery[drop_idx].last_seen_time;
                        g_gallery[keep_idx].last_seen_frame = g_gallery[drop_idx].last_seen_frame;
                    }

                    // Reassign all active tracks pointing to drop_id -> keep_id
                    for (auto& trk : g_tracks) {
                        if (trk.person_id == drop_id) {
                            trk.person_id = keep_id;
                            trk.color = g_gallery[keep_idx].color;
                            trk.feature = g_gallery[keep_idx].feature;
                        }
                    }

                    std::cout << "[MERGE FILTER] Deduplication: Consolidated duplicate Person #" << drop_id
                              << " into Person #" << keep_id << " (sim=" << (int)(max_cross_sim * 100.0f) << "%)\n";

                    g_gallery.erase(g_gallery.begin() + drop_idx);
                    merged = true; // Repeat until clean
                }
            }
        }
    }
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            EnterCriticalSection(&g_cs);
            std::vector<uint32_t> local_pixels = g_rgb32_buffer;
            OdMetadataPacket_t meta = g_latest_meta;
            uint32_t frame_id = g_current_frame_id;
            double fps = g_stream_fps;

            std::vector<ActiveTrack> tracks_copy = g_tracks;
            std::vector<PersonProfile> gallery_copy = g_gallery;
            std::vector<int8_t> latest_emb = g_raw_latest_embedding;
            uint32_t last_box = g_latest_reid_box;
            uint32_t last_reid_frame = g_latest_reid_frame;
            uint32_t total_reid = g_reid_total_count;
            LeaveCriticalSection(&g_cs);

            // Double buffering memory DC
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, TOTAL_W, TOTAL_H);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // Clear full canvas background
            RECT fullRect = {0, 0, TOTAL_W, TOTAL_H};
            HBRUSH bgBrush = CreateSolidBrush(RGB(15, 17, 23));
            FillRect(memDC, &fullRect, bgBrush);
            DeleteObject(bgBrush);

            // DIB section header to blit 256x256 image
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = FRAME_W;
            bmi.bmiHeader.biHeight = -FRAME_H; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            // 1. Top Header Banner
            RECT bannerRect = {0, 0, TOTAL_W, HEADER_H};
            HBRUSH bannerBrush = CreateSolidBrush(RGB(22, 27, 34));
            FillRect(memDC, &bannerRect, bannerBrush);
            DeleteObject(bannerBrush);

            HPEN divPen = CreatePen(PS_SOLID, 1, RGB(48, 54, 61));
            HPEN oldPen = (HPEN)SelectObject(memDC, divPen);
            MoveToEx(memDC, 0, HEADER_H - 1, NULL);
            LineTo(memDC, TOTAL_W, HEADER_H - 1);

            char bannerText[256];
            snprintf(bannerText, sizeof(bannerText),
                     "  STM32N6 EDGE-AI | Frame #%u | Video: %.1f FPS | YOLO: %u ms | Tracks: %u | Thresh: %.0f%%",
                     frame_id, fps, (unsigned int)meta.inference_ms, (unsigned int)tracks_copy.size(), REID_SIMILARITY_THRESHOLD * 100.0f);

            SetTextColor(memDC, RGB(0, 240, 255));
            SetBkMode(memDC, TRANSPARENT);
            DrawTextA(memDC, bannerText, -1, &bannerRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            // 2. Video Frame Blit (512x512)
            SetStretchBltMode(memDC, COLORONCOLOR);
            StretchDIBits(memDC,
                          0, HEADER_H, DISP_W, DISP_H,
                          0, 0, FRAME_W, FRAME_H,
                          local_pixels.data(),
                          &bmi,
                          DIB_RGB_COLORS,
                          SRCCOPY);

            // 3. Draw Bounding Boxes + Mutual-Exclusion Spatial Track Overlay
            auto now = std::chrono::steady_clock::now();

            // Match each detection box to the best active track (Greedy 1-to-1 matching)
            std::vector<int> box_to_track(meta.num_boxes, -1);
            std::vector<bool> track_used(tracks_copy.size(), false);

            for (uint8_t i = 0; i < meta.num_boxes; i++) {
                int best_t = -1;
                float best_overlap = 0.15f; // minimum IoU or center proximity
                for (size_t t = 0; t < tracks_copy.size(); t++) {
                    if (track_used[t]) continue;
                    float iou = compute_iou(meta.boxes[i], tracks_copy[t].box);
                    // Also consider center distance if boxes are small
                    float dx = meta.boxes[i].cx - tracks_copy[t].box.cx;
                    float dy = meta.boxes[i].cy - tracks_copy[t].box.cy;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    float score = iou + (dist < 0.15f ? (0.20f - dist) : 0.0f);

                    if (score > best_overlap) {
                        best_overlap = score;
                        best_t = (int)t;
                    }
                }
                if (best_t >= 0) {
                    box_to_track[i] = best_t;
                    track_used[best_t] = true;
                }
            }

            for (uint8_t i = 0; i < meta.num_boxes; i++) {
                int cx = (int)(meta.boxes[i].cx * DISP_W);
                int cy = (int)(meta.boxes[i].cy * DISP_H) + HEADER_H;
                int bw = (int)(meta.boxes[i].w * DISP_W);
                int bh = (int)(meta.boxes[i].h * DISP_H);

                int x1 = (std::max)(0, cx - bw / 2);
                int y1 = (std::max)(HEADER_H, cy - bh / 2);
                int x2 = (std::min)(DISP_W - 1, cx + bw / 2);
                int y2 = (std::min)(HEADER_H + DISP_H - 1, cy + bh / 2);

                int assigned_track = box_to_track[i];
                COLORREF box_color = RGB(160, 160, 160); // Neutral grey if unassigned
                char label[96];
                int conf_pct = (int)(meta.boxes[i].conf * 100.0f);

                if (assigned_track >= 0 && assigned_track < (int)tracks_copy.size()) {
                    const auto& trk = tracks_copy[assigned_track];
                    box_color = trk.color;
                    if (trk.person_id > 0) {
                        snprintf(label, sizeof(label), " Person #%d (%d%%) | Conf: %d%% ",
                                 trk.person_id, (int)(trk.sim * 100.0f), conf_pct);
                    } else {
                        snprintf(label, sizeof(label), " Track #%d [Scanning...] | Conf: %d%% ",
                                 trk.track_id, conf_pct);
                    }
                } else {
                    snprintf(label, sizeof(label), " Box #%u | Conf: %d%% ", i, conf_pct);
                }

                HPEN boxPen = CreatePen(PS_SOLID, 3, box_color);
                SelectObject(memDC, boxPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));

                Rectangle(memDC, x1, y1, x2, y2);

                // Label Banner above bounding box
                RECT labelRect = {x1, (std::max)(HEADER_H, y1 - 22), x1 + 205, y1};
                HBRUSH labelBg = CreateSolidBrush(box_color);
                FillRect(memDC, &labelRect, labelBg);
                DeleteObject(labelBg);

                SetTextColor(memDC, RGB(0, 0, 0));
                DrawTextA(memDC, label, -1, &labelRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

                // Bottom coordinates tag
                char coordStr[64];
                snprintf(coordStr, sizeof(coordStr), "cx=%.2f cy=%.2f w=%.2f h=%.2f",
                         meta.boxes[i].cx, meta.boxes[i].cy, meta.boxes[i].w, meta.boxes[i].h);
                RECT coordRect = {x1, y2, x1 + 185, y2 + 16};
                SetTextColor(memDC, RGB(200, 200, 200));
                DrawTextA(memDC, coordStr, -1, &coordRect, DT_SINGLELINE | DT_LEFT);

                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(boxPen);
            }

            // 4. Right Sidebar: ReID Gallery & Live Embedding Inspector
            RECT sidebarRect = {DISP_W, HEADER_H, TOTAL_W, TOTAL_H};
            HBRUSH sideBrush = CreateSolidBrush(RGB(18, 22, 30));
            FillRect(memDC, &sidebarRect, sideBrush);
            DeleteObject(sideBrush);

            // Vertical divider line between Video and Sidebar
            MoveToEx(memDC, DISP_W, HEADER_H, NULL);
            LineTo(memDC, DISP_W, TOTAL_H);

            // Sidebar Section 1: Gallery
            RECT titleRect = {DISP_W + 12, HEADER_H + 10, TOTAL_W - 12, HEADER_H + 28};
            SetTextColor(memDC, RGB(255, 255, 255));
            DrawTextA(memDC, "IDENTITIES IN GALLERY", -1, &titleRect, DT_SINGLELINE | DT_LEFT);

            // Divider
            MoveToEx(memDC, DISP_W + 10, HEADER_H + 32, NULL);
            LineTo(memDC, TOTAL_W - 10, HEADER_H + 32);

            int cardY = HEADER_H + 38;
            for (size_t i = 0; i < gallery_copy.size() && i < 4; i++) {
                const auto& p = gallery_copy[i];
                double age = std::chrono::duration<double>(now - p.last_seen_time).count();
                bool is_active = (age < 3.0);

                RECT cardRect = {DISP_W + 12, cardY, TOTAL_W - 12, cardY + 50};
                HBRUSH cardBg = CreateSolidBrush(is_active ? RGB(26, 33, 44) : RGB(22, 27, 34));
                FillRect(memDC, &cardRect, cardBg);
                DeleteObject(cardBg);

                HPEN borderPen = CreatePen(PS_SOLID, 1, is_active ? p.color : RGB(48, 54, 61));
                SelectObject(memDC, borderPen);
                SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
                Rectangle(memDC, cardRect.left, cardRect.top, cardRect.right, cardRect.bottom);
                SelectObject(memDC, oldPen);
                DeleteObject(borderPen);

                // Accent Pill
                RECT pillRect = {cardRect.left + 8, cardRect.top + 8, cardRect.left + 14, cardRect.bottom - 8};
                HBRUSH pillBrush = CreateSolidBrush(p.color);
                FillRect(memDC, &pillRect, pillBrush);
                DeleteObject(pillBrush);

                // Header
                char idHeader[64];
                snprintf(idHeader, sizeof(idHeader), "Person #%d  %s", p.id, is_active ? "[ACTIVE]" : "[LOST]");
                RECT idTextRect = {cardRect.left + 22, cardRect.top + 6, cardRect.right - 8, cardRect.top + 24};
                SetTextColor(memDC, is_active ? p.color : RGB(140, 140, 140));
                DrawTextA(memDC, idHeader, -1, &idTextRect, DT_SINGLELINE | DT_LEFT);

                // Stats
                char statsText[128];
                snprintf(statsText, sizeof(statsText), "Matches: %d  |  Last Sim: %d%%  |  Frame #%u",
                         p.match_count, (int)(p.last_similarity * 100.0f), p.last_seen_frame);
                RECT statsRect = {cardRect.left + 22, cardRect.top + 26, cardRect.right - 8, cardRect.bottom - 6};
                SetTextColor(memDC, RGB(139, 148, 158));
                DrawTextA(memDC, statsText, -1, &statsRect, DT_SINGLELINE | DT_LEFT);

                cardY += 56;
            }

            if (gallery_copy.empty()) {
                RECT noGalleryRect = {DISP_W + 12, cardY + 10, TOTAL_W - 12, cardY + 40};
                SetTextColor(memDC, RGB(110, 118, 129));
                DrawTextA(memDC, "Awaiting person detections...", -1, &noGalleryRect, DT_LEFT);
                cardY += 45;
            }

            // Sidebar Section 2: Live 128-Byte Embedding Inspector
            int embY = std::max(cardY + 10, HEADER_H + 250);
            RECT embTitleRect = {DISP_W + 12, embY, TOTAL_W - 12, embY + 18};
            SetTextColor(memDC, RGB(0, 240, 255));
            char embHeader[64];
            snprintf(embHeader, sizeof(embHeader), "LATEST 128-BYTE EMBEDDING (Box #%u)", last_box);
            DrawTextA(memDC, embHeader, -1, &embTitleRect, DT_SINGLELINE | DT_LEFT);

            MoveToEx(memDC, DISP_W + 10, embY + 22, NULL);
            LineTo(memDC, TOTAL_W - 10, embY + 22);

            // Display sample rows of 128-byte INT8 embedding
            int rowY = embY + 28;
            for (int row = 0; row < 8 && row * 16 < REID_EMBEDDING_DIM; row++) {
                char rowStr[128] = {};
                int offset = 0;
                offset += snprintf(rowStr + offset, sizeof(rowStr) - offset, "[%02d..%02d] ", row * 16, row * 16 + 15);
                for (int col = 0; col < 8; col++) {
                    int idx = row * 16 + col;
                    offset += snprintf(rowStr + offset, sizeof(rowStr) - offset, "%4d", (int)latest_emb[idx]);
                }
                RECT rowRect = {DISP_W + 12, rowY, TOTAL_W - 12, rowY + 16};
                SetTextColor(memDC, RGB(180, 190, 205));
                DrawTextA(memDC, rowStr, -1, &rowRect, DT_SINGLELINE | DT_LEFT);
                rowY += 16;
            }

            // Bottom Controls Banner
            RECT diagRect = {DISP_W + 12, TOTAL_H - 50, TOTAL_W - 12, TOTAL_H - 10};
            HBRUSH diagBg = CreateSolidBrush(RGB(13, 17, 23));
            FillRect(memDC, &diagRect, diagBg);
            DeleteObject(diagBg);

            char diagStr[128];
            snprintf(diagStr, sizeof(diagStr),
                     "[R] Reset Gallery Identities\n[ESC/Q] Exit Application");
            SetTextColor(memDC, RGB(110, 118, 129));
            DrawTextA(memDC, diagStr, -1, &diagRect, DT_LEFT);

            SelectObject(memDC, oldPen);
            DeleteObject(divPen);

            BitBlt(hdc, 0, 0, TOTAL_W, TOTAL_H, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == 'R' || wParam == 'r') {
                EnterCriticalSection(&g_cs);
                g_gallery.clear();
                g_tracks.clear();
                g_next_track_id = 1;
                LeaveCriticalSection(&g_cs);
                std::cout << "[*] ReID Gallery & Tracks Reset.\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == VK_ESCAPE || wParam == 'Q' || wParam == 'q') {
                g_running = false;
                PostQuitMessage(0);
            }
            return 0;
        }

        case WM_DESTROY:
            g_running = false;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

DWORD WINAPI NetworkThread(LPVOID lpParam) {
    (void)lpParam;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[!] WSAStartup failed\n";
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "[!] Socket creation failed\n";
        WSACleanup();
        return 1;
    }

    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(LISTEN_PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[!] Bind failed on port " << LISTEN_PORT << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "=========================================================\n";
    std::cout << "  STM32N6 Object Detection & ReID Viewer Server          \n";
    std::cout << "=========================================================\n";
    std::cout << "[+] UDP Server listening on port " << LISTEN_PORT << "...\n";
    std::cout << "[+] ReID Matching Threshold: " << (int)(REID_SIMILARITY_THRESHOLD * 100.0f) << "%\n";

    std::vector<uint8_t> recv_buf(2048);
    std::vector<uint8_t> raw_frame_565(FRAME_W * FRAME_H * 2, 0);

    OdMetadataPacket_t current_meta = {};
    uint32_t active_frame_id = 0;
    uint32_t chunks_received = 0;

    auto fps_start = std::chrono::steady_clock::now();
    uint32_t frame_count = 0;

    while (g_running) {
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        int bytes = recvfrom(sock, (char*)recv_buf.data(), (int)recv_buf.size(), 0, (sockaddr*)&client_addr, &client_len);
        if (bytes < 4) continue;

        uint32_t magic;
        memcpy(&magic, recv_buf.data(), sizeof(uint32_t));
        if (magic != STREAM_MAGIC) continue;

        uint8_t pkt_type = recv_buf[4];

        // 1. ReID Metadata Packet
        if (pkt_type == PKT_TYPE_REID_METADATA && bytes >= (int)(sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t))) {
            ReidMetadataPacket_t reid_pkt = {};
            memcpy(&reid_pkt, recv_buf.data(), std::min((size_t)bytes, sizeof(ReidMetadataPacket_t)));

            size_t emb_len = reid_pkt.embedding_len;
            if (emb_len == 0 || emb_len > REID_EMBEDDING_DIM) emb_len = REID_EMBEDDING_DIM;
            std::vector<float> norm_emb = normalize_int8_embedding(reid_pkt.embedding, emb_len);

            int box_idx = (int)reid_pkt.box_index;

            EnterCriticalSection(&g_cs);
            auto now_t = std::chrono::steady_clock::now();

            // Find matching ActiveTrack by spatial proximity to reid_pkt.box
            int matched_track_idx = -1;
            float best_match_score = 0.10f; // Minimum score threshold

            for (size_t t = 0; t < g_tracks.size(); t++) {
                float iou = compute_iou(reid_pkt.box, g_tracks[t].box);
                float dx = reid_pkt.box.cx - g_tracks[t].box.cx;
                float dy = reid_pkt.box.cy - g_tracks[t].box.cy;
                float dist = std::sqrt(dx * dx + dy * dy);
                float score = iou + (dist < 0.20f ? (0.25f - dist) : 0.0f);
                if (score > best_match_score) {
                    best_match_score = score;
                    matched_track_idx = (int)t;
                }
            }

            // If no existing track matched reid_pkt.box, create one
            if (matched_track_idx < 0) {
                ActiveTrack new_t = {};
                new_t.track_id = g_next_track_id++;
                new_t.person_id = 0;
                new_t.sim = 0.0f;
                new_t.color = RGB(160, 160, 160);
                new_t.box = reid_pkt.box;
                new_t.has_feature = false;
                new_t.observation_count = 0;
                new_t.last_seen = now_t;
                g_tracks.push_back(new_t);
                matched_track_idx = (int)g_tracks.size() - 1;
            }

            ActiveTrack& trk = g_tracks[matched_track_idx];
            trk.box = reid_pkt.box;
            trk.last_seen = now_t;

            // Check cosine similarity against confirmed gallery profiles
            int best_gallery_id = -1;
            float best_gallery_sim = -1.0f;
            int best_gallery_idx = -1;

            for (size_t i = 0; i < g_gallery.size(); i++) {
                // Mutual exclusion: Check if another active track is already assigned this gallery person_id
                bool in_use_by_other = false;
                for (size_t other_t = 0; other_t < g_tracks.size(); other_t++) {
                    if ((int)other_t != matched_track_idx && g_tracks[other_t].person_id == g_gallery[i].id) {
                        double age = std::chrono::duration<double>(now_t - g_tracks[other_t].last_seen).count();
                        if (age < 1.0) { // Actively seen within last 1 second
                            in_use_by_other = true;
                            break;
                        }
                    }
                }
                if (in_use_by_other) continue;

                float sim = compute_cosine_similarity(g_gallery[i].feature, norm_emb);
                if (sim > best_gallery_sim) {
                    best_gallery_sim = sim;
                    best_gallery_id = g_gallery[i].id;
                    best_gallery_idx = (int)i;
                }
            }

            if (best_gallery_sim >= REID_SIMILARITY_THRESHOLD && best_gallery_idx >= 0) {
                // Match confirmed in Gallery: update gallery template via EMA
                for (size_t i = 0; i < norm_emb.size(); i++) {
                    g_gallery[best_gallery_idx].feature[i] = 0.90f * g_gallery[best_gallery_idx].feature[i] + 0.10f * norm_emb[i];
                }
                float s_sq = 0.0f;
                for (float v : g_gallery[best_gallery_idx].feature) s_sq += v * v;
                float n_val = std::sqrt(s_sq);
                if (n_val > 1e-6f) {
                    for (float &v : g_gallery[best_gallery_idx].feature) v /= n_val;
                }

                // Add to rolling history of recent embeddings
                g_gallery[best_gallery_idx].history.push_back(norm_emb);
                if (g_gallery[best_gallery_idx].history.size() > 10) {
                    g_gallery[best_gallery_idx].history.erase(g_gallery[best_gallery_idx].history.begin());
                }

                g_gallery[best_gallery_idx].match_count++;
                g_gallery[best_gallery_idx].last_seen_frame = reid_pkt.frame_id;
                g_gallery[best_gallery_idx].last_similarity = best_gallery_sim;
                g_gallery[best_gallery_idx].last_seen_time = now_t;

                trk.person_id = best_gallery_id;
                trk.sim = best_gallery_sim;
                trk.color = g_gallery[best_gallery_idx].color;
                trk.feature = g_gallery[best_gallery_idx].feature;
                trk.has_feature = true;
                trk.observation_count++;

                std::cout << "[RX REID] Track #" << trk.track_id << " (crop box " << box_idx << ") | frame #" << reid_pkt.frame_id
                          << " ==> [MATCH] Person #" << best_gallery_id
                          << " (sim=" << (int)(best_gallery_sim * 100.0f) << "% >= "
                          << (int)(REID_SIMILARITY_THRESHOLD * 100.0f) << "%)\n";
            } else {
                // No match with existing gallery profiles
                if (!trk.has_feature) {
                    trk.feature = norm_emb;
                    trk.has_feature = true;
                    trk.observation_count = 1;
                    trk.sim = 1.0f;
                    std::cout << "[RX REID] Track #" << trk.track_id << " (crop box " << box_idx << ") | frame #" << reid_pkt.frame_id
                              << " ==> [NEW CANDIDATE] observation 1/" << REID_CONFIRMATION_COUNT << "\n";
                } else {
                    float self_sim = compute_cosine_similarity(trk.feature, norm_emb);
                    if (self_sim >= 0.85f) {
                        for (size_t i = 0; i < norm_emb.size(); i++) {
                            trk.feature[i] = 0.90f * trk.feature[i] + 0.10f * norm_emb[i];
                        }
                        float s_sq = 0.0f;
                        for (float v : trk.feature) s_sq += v * v;
                        float n_val = std::sqrt(s_sq);
                        if (n_val > 1e-6f) {
                            for (float &v : trk.feature) v /= n_val;
                        }
                        trk.observation_count++;
                        trk.sim = self_sim;

                        if (trk.observation_count >= REID_CONFIRMATION_COUNT && trk.person_id == 0) {
                            // Pre-promotion cross-check: Does this candidate match ANY existing gallery person's history?
                            int pre_match_id = -1;
                            int pre_match_idx = -1;
                            float pre_match_sim = -1.0f;
                            for (size_t gi = 0; gi < g_gallery.size(); gi++) {
                                float s_mean = compute_cosine_similarity(g_gallery[gi].feature, trk.feature);
                                if (s_mean > pre_match_sim) {
                                    pre_match_sim = s_mean;
                                    pre_match_id = g_gallery[gi].id;
                                    pre_match_idx = (int)gi;
                                }
                                for (const auto& h_emb : g_gallery[gi].history) {
                                    float s_hist = compute_cosine_similarity(h_emb, trk.feature);
                                    if (s_hist > pre_match_sim) {
                                        pre_match_sim = s_hist;
                                        pre_match_id = g_gallery[gi].id;
                                        pre_match_idx = (int)gi;
                                    }
                                }
                            }

                            if (pre_match_sim >= 0.88f && pre_match_idx >= 0) {
                                // Snap candidate to existing person instead of spawning a duplicate
                                trk.person_id = pre_match_id;
                                trk.color = g_gallery[pre_match_idx].color;
                                trk.feature = g_gallery[pre_match_idx].feature;
                                g_gallery[pre_match_idx].history.push_back(trk.feature);
                                if (g_gallery[pre_match_idx].history.size() > 10) {
                                    g_gallery[pre_match_idx].history.erase(g_gallery[pre_match_idx].history.begin());
                                }
                                std::cout << "[RX REID] Track #" << trk.track_id
                                          << " ==> Snapped to existing Person #" << pre_match_id
                                          << " (pre-promotion sim=" << (int)(pre_match_sim * 100.0f) << "%)\n";
                            } else {
                                // Confirmed new person! Promote to gallery
                                int new_id = (int)g_gallery.size() + 1;
                                COLORREF color = ID_COLORS[(new_id - 1) % NUM_ID_COLORS];
                                PersonProfile p;
                                p.id = new_id;
                                p.feature = trk.feature;
                                p.history.push_back(trk.feature);
                                p.color = color;
                                p.last_seen_frame = reid_pkt.frame_id;
                                p.match_count = trk.observation_count;
                                p.last_similarity = self_sim;
                                p.last_seen_time = now_t;
                                g_gallery.push_back(p);

                                trk.person_id = new_id;
                                trk.color = color;

                                std::cout << "[RX REID] Track #" << trk.track_id << " | frame #" << reid_pkt.frame_id
                                          << " ==> [CONFIRMED PERSON #" << new_id << "] after "
                                          << REID_CONFIRMATION_COUNT << " observations (self_sim="
                                          << (int)(self_sim * 100.0f) << "%)\n";
                            }
                        } else {
                            std::cout << "[RX REID] Track #" << trk.track_id << " | frame #" << reid_pkt.frame_id
                                      << " ==> [CANDIDATE] observation " << trk.observation_count
                                      << "/" << REID_CONFIRMATION_COUNT << " (self_sim="
                                      << (int)(self_sim * 100.0f) << "%)\n";
                        }
                    } else {
                        // Sudden feature change on track, reset template
                        trk.feature = norm_emb;
                        trk.observation_count = 1;
                    }
                }
            }

            // Periodically run cross-identity deduplication
            reconcile_and_merge_gallery(now_t);

            g_raw_latest_embedding.assign(reid_pkt.embedding, reid_pkt.embedding + emb_len);
            g_latest_reid_box = box_idx;
            g_latest_reid_frame = reid_pkt.frame_id;
            g_reid_total_count++;
            LeaveCriticalSection(&g_cs);

            if (g_hwnd) {
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        }
        // 2. OD Metadata Packet
        else if (pkt_type == PKT_TYPE_OD_METADATA && bytes >= (int)sizeof(OdMetadataPacket_t)) {
            memcpy(&current_meta, recv_buf.data(), sizeof(OdMetadataPacket_t));

            EnterCriticalSection(&g_cs);
            auto now_t = std::chrono::steady_clock::now();

            // Update active tracks with new frame detections (greedy matching)
            std::vector<bool> track_matched(g_tracks.size(), false);
            for (uint8_t i = 0; i < current_meta.num_boxes; i++) {
                int best_t = -1;
                float best_overlap = 0.15f;
                for (size_t t = 0; t < g_tracks.size(); t++) {
                    if (track_matched[t]) continue;
                    float iou = compute_iou(current_meta.boxes[i], g_tracks[t].box);
                    float dx = current_meta.boxes[i].cx - g_tracks[t].box.cx;
                    float dy = current_meta.boxes[i].cy - g_tracks[t].box.cy;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    float score = iou + (dist < 0.15f ? (0.20f - dist) : 0.0f);
                    if (score > best_overlap) {
                        best_overlap = score;
                        best_t = (int)t;
                    }
                }
                if (best_t >= 0) {
                    g_tracks[best_t].box = current_meta.boxes[i];
                    g_tracks[best_t].last_seen = now_t;
                    track_matched[best_t] = true;
                } else {
                    // Create new active track
                    ActiveTrack new_t = {};
                    new_t.track_id = g_next_track_id++;
                    new_t.person_id = 0;
                    new_t.sim = 0.0f;
                    new_t.color = RGB(160, 160, 160);
                    new_t.box = current_meta.boxes[i];
                    new_t.has_feature = false;
                    new_t.observation_count = 0;
                    new_t.last_seen = now_t;
                    g_tracks.push_back(new_t);
                }
            }

            // Prune tracks unseen for > 2.0s
            g_tracks.erase(
                std::remove_if(g_tracks.begin(), g_tracks.end(),
                    [&](const ActiveTrack& trk) {
                        return std::chrono::duration<double>(now_t - trk.last_seen).count() > 2.0;
                    }),
                g_tracks.end()
            );

            LeaveCriticalSection(&g_cs);
        }
        // 3. Video Chunk Packet
        else if (pkt_type == PKT_TYPE_VIDEO_CHUNK && bytes >= (int)sizeof(VideoChunkHeader_t)) {
            VideoChunkHeader_t hdr;
            memcpy(&hdr, recv_buf.data(), sizeof(VideoChunkHeader_t));

            int payload_len = hdr.payload_len;
            if (sizeof(VideoChunkHeader_t) + payload_len <= (size_t)bytes) {
                int stride = (hdr.total_chunks <= 100) ? 1400 : 1024;
                int offset = hdr.chunk_idx * stride;

                if (offset + payload_len <= (int)raw_frame_565.size()) {
                    memcpy(raw_frame_565.data() + offset,
                           recv_buf.data() + sizeof(VideoChunkHeader_t),
                           payload_len);
                }

                if (hdr.frame_id != active_frame_id) {
                    active_frame_id = hdr.frame_id;
                    chunks_received = 0;
                }
                chunks_received++;

                // Frame complete or last chunk received
                if (chunks_received >= hdr.total_chunks || hdr.chunk_idx == hdr.total_chunks - 1) {
                    EnterCriticalSection(&g_cs);
                    const uint16_t* p565 = (const uint16_t*)raw_frame_565.data();
                    for (int i = 0; i < FRAME_W * FRAME_H; i++) {
                        uint16_t c = p565[i];
                        uint8_t cr = (c >> 11) & 0x1F;
                        uint8_t cg = (c >> 5) & 0x3F;
                        uint8_t cb = c & 0x1F;
                        uint8_t r = (cr * 527 + 23) >> 6;
                        uint8_t g = (cg * 259 + 33) >> 6;
                        uint8_t b = (cb * 527 + 23) >> 6;
                        g_rgb32_buffer[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                    }
                    g_latest_meta = current_meta;
                    g_current_frame_id = hdr.frame_id;
                    g_has_frame = true;
                    LeaveCriticalSection(&g_cs);

                    // Compute FPS
                    frame_count++;
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - fps_start).count();
                    if (elapsed >= 1.0) {
                        EnterCriticalSection(&g_cs);
                        g_stream_fps = frame_count / elapsed;
                        LeaveCriticalSection(&g_cs);
                        frame_count = 0;
                        fps_start = now;
                    }

                    // Trigger Repaint
                    if (g_hwnd) {
                        InvalidateRect(g_hwnd, NULL, FALSE);
                    }
                }
            }
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}

int main() {
    InitializeCriticalSection(&g_cs);

    HANDLE hThread = CreateThread(NULL, 0, NetworkThread, NULL, 0, NULL);
    if (!hThread) {
        std::cerr << "[!] Failed to start network thread\n";
        return 1;
    }

    // Register Win32 Window Class
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "STM32N6_ReID_Viewer_Class";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    if (!RegisterClassExA(&wc)) {
        std::cerr << "[!] RegisterClassEx failed\n";
        return 1;
    }

    // Window size: 832x548 (512x512 Video + 320px Sidebar + 36px Top Banner)
    RECT wr = {0, 0, TOTAL_W, TOTAL_H};
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwnd = CreateWindowExA(
        0,
        "STM32N6_ReID_Viewer_Class",
        "STM32N6 Object Detection & ReID Real-Time Viewer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) {
        std::cerr << "[!] CreateWindowEx failed\n";
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    std::cout << "[+] Window created successfully.\n";
    std::cout << "    [R]     Reset ReID Gallery\n";
    std::cout << "    [ESC/Q] Exit Application\n";

    // Win32 Message Loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    WaitForSingleObject(hThread, 1000);
    CloseHandle(hThread);
    DeleteCriticalSection(&g_cs);

    return 0;
}
