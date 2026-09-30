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
#include <atomic>

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

// Real-time tunable threshold (default 75%, adjustable live from 40% to 98%)
static std::atomic<float> g_reid_similarity_threshold(0.75f);

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
    COLORREF color;
    uint32_t last_seen_frame;
    int match_count;
    float last_similarity;
    std::chrono::steady_clock::time_point last_seen_time;
};

// ReID match score for a specific gallery person
struct PersonMatchScore {
    int person_id;
    float sim;
};

// Spatial Multi-Object Track Structure (IoU tracking)
struct ActiveTrack {
    int track_id;               // Sequential Track ID (1, 2, ...)
    int person_id;              // Confirmed Gallery Person ID (1, 2, ...) or 0 if unassigned
    float sim;                  // Last match similarity
    COLORREF color;             // Color associated with person or neutral
    DetectionBox_t box;         // Last known bounding box coordinates (cx, cy, w, h)
    std::vector<PersonMatchScore> all_scores; // Similarity score with each person in gallery
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

// (Cross-identity gallery merging filter removed per configuration)



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
                     "  STM32N6 EDGE-AI | Frame #%u | Video: %.1f FPS | YOLO: %u ms | Tracks: %u | Thresh: %.0f%% (Scroll to tune)",
                     frame_id, fps, (unsigned int)meta.inference_ms, (unsigned int)tracks_copy.size(), g_reid_similarity_threshold.load() * 100.0f);

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

                // Corner Badge: small text list of similarity scores against all gallery persons (e.g. p1 - 85%, p2 - 42%)
                if (assigned_track >= 0 && assigned_track < (int)tracks_copy.size()) {
                    const auto& trk = tracks_copy[assigned_track];
                    if (!trk.all_scores.empty()) {
                        int num_scores = (int)trk.all_scores.size();
                        int badge_w = 72;
                        int badge_h = num_scores * 15 + 4;
                        // Position badge inside the top-right corner of the bounding box (or top-left if box is near right edge)
                        int badge_x = (x2 - badge_w - 4 >= x1) ? (x2 - badge_w - 4) : (x1 + 4);
                        int badge_y = y1 + 4;

                        RECT badgeRect = {badge_x, badge_y, badge_x + badge_w, badge_y + badge_h};
                        HBRUSH badgeBg = CreateSolidBrush(RGB(15, 20, 28));
                        FillRect(memDC, &badgeRect, badgeBg);
                        DeleteObject(badgeBg);

                        HPEN badgePen = CreatePen(PS_SOLID, 1, RGB(48, 54, 61));
                        SelectObject(memDC, badgePen);
                        SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
                        Rectangle(memDC, badgeRect.left, badgeRect.top, badgeRect.right, badgeRect.bottom);
                        SelectObject(memDC, oldPen);
                        DeleteObject(badgePen);

                        float live_thresh = g_reid_similarity_threshold.load();
                        int row_y = badge_y + 2;
                        for (const auto& ms : trk.all_scores) {
                            char scoreStr[32];
                            snprintf(scoreStr, sizeof(scoreStr), "p%d - %d%%", ms.person_id, (int)(ms.sim * 100.0f));
                            RECT scoreRowRect = {badge_x + 5, row_y, badge_x + badge_w - 3, row_y + 14};
                            // Highlight in bright green if >= threshold, else light grey
                            COLORREF textColor = (ms.sim >= live_thresh) ? RGB(50, 255, 120) : RGB(180, 190, 200);
                            SetTextColor(memDC, textColor);
                            DrawTextA(memDC, scoreStr, -1, &scoreRowRect, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
                            row_y += 15;
                        }
                    }
                }

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
            for (size_t i = 0; i < gallery_copy.size() && i < 2; i++) {
                const auto& p = gallery_copy[i];
                double age = std::chrono::duration<double>(now - p.last_seen_time).count();
                bool is_active = (age < 3.0);

                RECT cardRect = {DISP_W + 12, cardY, TOTAL_W - 12, cardY + 44};
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
                RECT pillRect = {cardRect.left + 8, cardRect.top + 6, cardRect.left + 14, cardRect.bottom - 6};
                HBRUSH pillBrush = CreateSolidBrush(p.color);
                FillRect(memDC, &pillRect, pillBrush);
                DeleteObject(pillBrush);

                // Header
                char idHeader[64];
                snprintf(idHeader, sizeof(idHeader), "Person #%d  %s", p.id, is_active ? "[ACTIVE]" : "[LOST]");
                RECT idTextRect = {cardRect.left + 22, cardRect.top + 4, cardRect.right - 8, cardRect.top + 20};
                SetTextColor(memDC, is_active ? p.color : RGB(140, 140, 140));
                DrawTextA(memDC, idHeader, -1, &idTextRect, DT_SINGLELINE | DT_LEFT);

                // Stats
                char statsText[128];
                snprintf(statsText, sizeof(statsText), "Matches: %d  |  Last Sim: %d%%",
                         p.match_count, (int)(p.last_similarity * 100.0f));
                RECT statsRect = {cardRect.left + 22, cardRect.top + 22, cardRect.right - 8, cardRect.bottom - 4};
                SetTextColor(memDC, RGB(139, 148, 158));
                DrawTextA(memDC, statsText, -1, &statsRect, DT_SINGLELINE | DT_LEFT);

                cardY += 48;
            }

            if (gallery_copy.empty()) {
                RECT noGalleryRect = {DISP_W + 12, cardY + 4, TOTAL_W - 12, cardY + 28};
                SetTextColor(memDC, RGB(110, 118, 129));
                DrawTextA(memDC, "Awaiting person detections...", -1, &noGalleryRect, DT_LEFT);
                cardY += 32;
            }

            // Sidebar Section 2: Real-time Tunable Matching Threshold Card
            float cur_thresh = g_reid_similarity_threshold.load();
            int ctrlY = (std::max)(cardY + 8, HEADER_H + 95);
            RECT ctrlCard = {DISP_W + 12, ctrlY, TOTAL_W - 12, ctrlY + 100};
            HBRUSH ctrlBg = CreateSolidBrush(RGB(22, 27, 34));
            FillRect(memDC, &ctrlCard, ctrlBg);
            DeleteObject(ctrlBg);

            HPEN ctrlBorder = CreatePen(PS_SOLID, 1, RGB(0, 180, 216));
            SelectObject(memDC, ctrlBorder);
            SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
            Rectangle(memDC, ctrlCard.left, ctrlCard.top, ctrlCard.right, ctrlCard.bottom);
            SelectObject(memDC, oldPen);
            DeleteObject(ctrlBorder);

            // Threshold Title & Value
            char threshTitle[128];
            snprintf(threshTitle, sizeof(threshTitle), "MATCH THRESHOLD: %d%%", (int)(cur_thresh * 100.0f));
            RECT threshTitleRect = {ctrlCard.left + 10, ctrlY + 8, ctrlCard.right - 10, ctrlY + 26};
            SetTextColor(memDC, RGB(0, 240, 255));
            DrawTextA(memDC, threshTitle, -1, &threshTitleRect, DT_SINGLELINE | DT_LEFT);

            RECT hintRect = {ctrlCard.left + 10, ctrlY + 24, ctrlCard.right - 10, ctrlY + 38};
            SetTextColor(memDC, RGB(139, 148, 158));
            DrawTextA(memDC, "Scroll wheel / click buttons / drag", -1, &hintRect, DT_SINGLELINE | DT_LEFT);

            // [-] Button
            RECT btnMinus = {ctrlCard.left + 10, ctrlY + 42, ctrlCard.left + 42, ctrlY + 66};
            HBRUSH btnMinusBg = CreateSolidBrush(RGB(35, 42, 54));
            FillRect(memDC, &btnMinus, btnMinusBg);
            DeleteObject(btnMinusBg);
            HPEN btnPen = CreatePen(PS_SOLID, 1, RGB(68, 76, 86));
            SelectObject(memDC, btnPen);
            SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));
            Rectangle(memDC, btnMinus.left, btnMinus.top, btnMinus.right, btnMinus.bottom);
            SetTextColor(memDC, RGB(255, 255, 255));
            DrawTextA(memDC, "-", -1, &btnMinus, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

            // Slider Track
            int trackLeft = ctrlCard.left + 50;
            int trackRight = ctrlCard.right - 50;
            int trackWidth = trackRight - trackLeft;
            RECT sliderTrack = {trackLeft, ctrlY + 50, trackRight, ctrlY + 58};
            HBRUSH trackBg = CreateSolidBrush(RGB(40, 48, 60));
            FillRect(memDC, &sliderTrack, trackBg);
            DeleteObject(trackBg);

            // Fill Track up to current threshold
            float fillRatio = (cur_thresh - 0.40f) / (0.95f - 0.40f);
            if (fillRatio < 0.0f) fillRatio = 0.0f;
            if (fillRatio > 1.0f) fillRatio = 1.0f;
            int fillW = (int)(fillRatio * trackWidth);
            RECT sliderFill = {trackLeft, ctrlY + 50, trackLeft + fillW, ctrlY + 58};
            HBRUSH fillBg = CreateSolidBrush(RGB(0, 200, 240));
            FillRect(memDC, &sliderFill, fillBg);
            DeleteObject(fillBg);

            // Slider Handle knob
            RECT knob = {trackLeft + fillW - 4, ctrlY + 46, trackLeft + fillW + 4, ctrlY + 62};
            HBRUSH knobBrush = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(memDC, &knob, knobBrush);
            DeleteObject(knobBrush);

            // [+] Button
            RECT btnPlus = {ctrlCard.right - 42, ctrlY + 42, ctrlCard.right - 10, ctrlY + 66};
            HBRUSH btnPlusBg = CreateSolidBrush(RGB(35, 42, 54));
            FillRect(memDC, &btnPlus, btnPlusBg);
            DeleteObject(btnPlusBg);
            Rectangle(memDC, btnPlus.left, btnPlus.top, btnPlus.right, btnPlus.bottom);
            SetTextColor(memDC, RGB(255, 255, 255));
            DrawTextA(memDC, "+", -1, &btnPlus, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            SelectObject(memDC, oldPen);
            DeleteObject(btnPen);

            // Preset Buttons: [60%] [70%] [75%] [80%] [88%]
            const int presets[] = {60, 70, 75, 80, 88};
            int pStartX = ctrlCard.left + 10;
            int pW = 50;
            int pGap = 6;
            for (int pi = 0; pi < 5; pi++) {
                RECT pRect = {pStartX + pi * (pW + pGap), ctrlY + 72, pStartX + pi * (pW + pGap) + pW, ctrlY + 92};
                bool is_selected = (std::abs(cur_thresh * 100.0f - presets[pi]) < 1.0f);
                HBRUSH pBg = CreateSolidBrush(is_selected ? RGB(0, 119, 182) : RGB(30, 36, 46));
                FillRect(memDC, &pRect, pBg);
                DeleteObject(pBg);
                char pTxt[16];
                snprintf(pTxt, sizeof(pTxt), "%d%%", presets[pi]);
                SetTextColor(memDC, is_selected ? RGB(255, 255, 255) : RGB(170, 180, 195));
                DrawTextA(memDC, pTxt, -1, &pRect, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            }

            // Sidebar Section 3: Live 128-Byte Embedding Inspector
            int embY = ctrlY + 108;
            RECT embTitleRect = {DISP_W + 12, embY, TOTAL_W - 12, embY + 18};
            SetTextColor(memDC, RGB(0, 240, 255));
            char embHeader[64];
            snprintf(embHeader, sizeof(embHeader), "LATEST 128-BYTE EMBEDDING (Box #%u)", last_box);
            DrawTextA(memDC, embHeader, -1, &embTitleRect, DT_SINGLELINE | DT_LEFT);

            MoveToEx(memDC, DISP_W + 10, embY + 22, NULL);
            LineTo(memDC, TOTAL_W - 10, embY + 22);

            // Display sample rows of 128-byte INT8 embedding
            int rowY = embY + 26;
            for (int row = 0; row < 7 && row * 16 < REID_EMBEDDING_DIM; row++) {
                char rowStr[128] = {};
                int offset = 0;
                offset += snprintf(rowStr + offset, sizeof(rowStr) - offset, "[%02d..%02d] ", row * 16, row * 16 + 15);
                for (int col = 0; col < 8; col++) {
                    int idx = row * 16 + col;
                    offset += snprintf(rowStr + offset, sizeof(rowStr) - offset, "%4d", (int)latest_emb[idx]);
                }
                RECT rowRect = {DISP_W + 12, rowY, TOTAL_W - 12, rowY + 15};
                SetTextColor(memDC, RGB(180, 190, 205));
                DrawTextA(memDC, rowStr, -1, &rowRect, DT_SINGLELINE | DT_LEFT);
                rowY += 15;
            }

            // Bottom Controls Banner
            RECT diagRect = {DISP_W + 12, TOTAL_H - 52, TOTAL_W - 12, TOTAL_H - 8};
            HBRUSH diagBg = CreateSolidBrush(RGB(13, 17, 23));
            FillRect(memDC, &diagRect, diagBg);
            DeleteObject(diagBg);

            char diagStr[128];
            snprintf(diagStr, sizeof(diagStr),
                     "[UP/DN/Wheel] Thresh  [0] Reset Thresh\n[R] Reset Gallery     [ESC/Q] Exit");
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

        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam);
            int my = HIWORD(lParam);

            // Check if clicked in threshold card (x: DISP_W + 12 to TOTAL_W - 12)
            if (mx >= DISP_W + 12 && mx <= TOTAL_W - 12 && my >= 130 && my <= 260) {
                float cur = g_reid_similarity_threshold.load();

                // 1. [-] Button (x: DISP_W + 22 to DISP_W + 54, y: 172 to 198)
                if (mx >= DISP_W + 22 && mx <= DISP_W + 54 && my >= 170 && my <= 200) {
                    cur -= 0.02f;
                }
                // 2. [+] Button (x: TOTAL_W - 54 to TOTAL_W - 22, y: 172 to 198)
                else if (mx >= TOTAL_W - 54 && mx <= TOTAL_W - 22 && my >= 170 && my <= 200) {
                    cur += 0.02f;
                }
                // 3. Slider Track (x: DISP_W + 62 to TOTAL_W - 62, y: 172 to 200)
                else if (mx >= DISP_W + 62 && mx <= TOTAL_W - 62 && my >= 170 && my <= 200) {
                    float ratio = (float)(mx - (DISP_W + 62)) / (float)((TOTAL_W - 62) - (DISP_W + 62));
                    cur = 0.40f + ratio * (0.95f - 0.40f);
                }
                // 4. Presets (y: 202 to 226): [60%] [70%] [75%] [80%] [88%]
                else if (my >= 202 && my <= 226) {
                    int pStartX = DISP_W + 22;
                    int pW = 50;
                    int pGap = 6;
                    const float preset_vals[] = {0.60f, 0.70f, 0.75f, 0.80f, 0.88f};
                    for (int pi = 0; pi < 5; pi++) {
                        int bx = pStartX + pi * (pW + pGap);
                        if (mx >= bx && mx <= bx + pW) {
                            cur = preset_vals[pi];
                            break;
                        }
                    }
                }

                cur = (std::max)(0.40f, (std::min)(0.98f, cur));
                g_reid_similarity_threshold.store(cur);
                std::cout << "[CONFIG] ReID Threshold changed to " << (int)(cur * 100.0f) << "%\n";
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            float cur = g_reid_similarity_threshold.load();
            if (zDelta > 0) cur += 0.01f;
            else if (zDelta < 0) cur -= 0.01f;
            cur = (std::max)(0.40f, (std::min)(0.98f, cur));
            g_reid_similarity_threshold.store(cur);
            std::cout << "[CONFIG] ReID Threshold changed to " << (int)(cur * 100.0f) << "%\n";
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_KEYDOWN: {
            if (wParam == VK_UP || wParam == VK_RIGHT || wParam == VK_OEM_PLUS || wParam == 0xBB) {
                float cur = (std::min)(0.98f, g_reid_similarity_threshold.load() + 0.01f);
                g_reid_similarity_threshold.store(cur);
                std::cout << "[CONFIG] ReID Threshold set to " << (int)(cur * 100.0f) << "%\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_OEM_MINUS || wParam == 0xBD) {
                float cur = (std::max)(0.40f, g_reid_similarity_threshold.load() - 0.01f);
                g_reid_similarity_threshold.store(cur);
                std::cout << "[CONFIG] ReID Threshold set to " << (int)(cur * 100.0f) << "%\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == VK_PRIOR) { // Page Up
                float cur = (std::min)(0.98f, g_reid_similarity_threshold.load() + 0.05f);
                g_reid_similarity_threshold.store(cur);
                std::cout << "[CONFIG] ReID Threshold set to " << (int)(cur * 100.0f) << "%\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == VK_NEXT) { // Page Down
                float cur = (std::max)(0.40f, g_reid_similarity_threshold.load() - 0.05f);
                g_reid_similarity_threshold.store(cur);
                std::cout << "[CONFIG] ReID Threshold set to " << (int)(cur * 100.0f) << "%\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == '0') { // Reset to default 75%
                g_reid_similarity_threshold.store(0.75f);
                std::cout << "[CONFIG] ReID Threshold reset to default 75%\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == 'R' || wParam == 'r') {
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
    std::cout << "[+] ReID Matching Threshold: " << (int)(g_reid_similarity_threshold.load() * 100.0f) << "%\n";
    std::cout << "    Use Mouse Wheel or Up/Down arrows to adjust in real-time!\n";

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
                new_t.last_seen = now_t;
                g_tracks.push_back(new_t);
                matched_track_idx = (int)g_tracks.size() - 1;
            }

            ActiveTrack& trk = g_tracks[matched_track_idx];
            trk.box = reid_pkt.box;
            trk.last_seen = now_t;

            // Pure Cosine Similarity matching against gallery profiles using dynamic threshold
            float live_threshold = g_reid_similarity_threshold.load();
            int best_gallery_id = -1;
            float best_gallery_sim = -1.0f;
            int best_gallery_idx = -1;

            trk.all_scores.clear();
            std::string scores_log = "[";

            for (size_t i = 0; i < g_gallery.size(); i++) {
                float sim = compute_cosine_similarity(g_gallery[i].feature, norm_emb);
                trk.all_scores.push_back({g_gallery[i].id, sim});

                char scBuf[32];
                snprintf(scBuf, sizeof(scBuf), "p%d:%d%%%s", g_gallery[i].id, (int)(sim * 100.0f), (i + 1 < g_gallery.size()) ? ", " : "");
                scores_log += scBuf;

                // Mutual exclusion: Ensure two active tracks don't claim the same person simultaneously
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

                if (sim > best_gallery_sim) {
                    best_gallery_sim = sim;
                    best_gallery_id = g_gallery[i].id;
                    best_gallery_idx = (int)i;
                }
            }
            scores_log += "]";

            if (best_gallery_sim >= live_threshold && best_gallery_idx >= 0) {
                // Match confirmed in Gallery: update gallery template feature via simple EMA
                for (size_t i = 0; i < norm_emb.size(); i++) {
                    g_gallery[best_gallery_idx].feature[i] = 0.85f * g_gallery[best_gallery_idx].feature[i] + 0.15f * norm_emb[i];
                }
                float s_sq = 0.0f;
                for (float v : g_gallery[best_gallery_idx].feature) s_sq += v * v;
                float n_val = std::sqrt(s_sq);
                if (n_val > 1e-6f) {
                    for (float &v : g_gallery[best_gallery_idx].feature) v /= n_val;
                }

                g_gallery[best_gallery_idx].match_count++;
                g_gallery[best_gallery_idx].last_seen_frame = reid_pkt.frame_id;
                g_gallery[best_gallery_idx].last_similarity = best_gallery_sim;
                g_gallery[best_gallery_idx].last_seen_time = now_t;

                trk.person_id = best_gallery_id;
                trk.sim = best_gallery_sim;
                trk.color = g_gallery[best_gallery_idx].color;

                std::cout << "[RX REID] Track #" << trk.track_id << " (crop box " << box_idx << ") | frame #" << reid_pkt.frame_id
                          << " " << scores_log << " ==> [MATCH] Person #" << best_gallery_id
                          << " (sim=" << (int)(best_gallery_sim * 100.0f) << "% >= "
                          << (int)(live_threshold * 100.0f) << "%)\n";
            } else {
                // Direct new person creation without waiting or buffering
                int new_id = (int)g_gallery.size() + 1;
                COLORREF color = ID_COLORS[(new_id - 1) % NUM_ID_COLORS];
                PersonProfile p;
                p.id = new_id;
                p.feature = norm_emb;
                p.color = color;
                p.last_seen_frame = reid_pkt.frame_id;
                p.match_count = 1;
                p.last_similarity = (best_gallery_sim > 0.0f) ? best_gallery_sim : 1.0f;
                p.last_seen_time = now_t;
                g_gallery.push_back(p);

                trk.person_id = new_id;
                trk.sim = 1.0f;
                trk.color = color;

                std::cout << "[RX REID] Track #" << trk.track_id << " (crop box " << box_idx << ") | frame #" << reid_pkt.frame_id
                          << " " << scores_log << " ==> [NEW PERSON #" << new_id << "] (best_sim="
                          << (int)(best_gallery_sim * 100.0f) << "% < "
                          << (int)(live_threshold * 100.0f) << "%)\n";
            }

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
