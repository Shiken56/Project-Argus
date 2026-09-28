#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <cmath>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define STREAM_MAGIC 0x54524F4EU
#define PKT_TYPE_VIDEO_CHUNK 0x01
#define PKT_TYPE_OD_METADATA 0x02
#define PKT_TYPE_REID_METADATA 0x03

#define LISTEN_PORT 5000
#define OD_MAX_BOXES 10
#define REID_EMBEDDING_DIM 128
#define REID_SIMILARITY_THRESHOLD 0.58f

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
    uint8_t pkt_type;
    uint8_t num_boxes;
    uint16_t inference_ms;
    uint32_t frame_id;
    uint16_t img_width;
    uint16_t img_height;
    DetectionBox_t boxes[OD_MAX_BOXES];
};

struct VideoChunkHeader_t {
    uint32_t magic;
    uint8_t pkt_type;
    uint8_t reserved;
    uint16_t chunk_idx;
    uint16_t total_chunks;
    uint16_t payload_len;
    uint32_t frame_id;
};

struct ReidMetadataPacket_t {
    uint32_t magic;          // STREAM_MAGIC (0x54524F4E)
    uint8_t  pkt_type;       // PKT_TYPE_REID_METADATA (3)
    uint8_t  box_index;      // 0 = top-1 detection
    uint16_t embedding_len;  // 128
    uint32_t frame_id;       // Synchronized with video frame_id
    int8_t   embedding[REID_EMBEDDING_DIM]; // Signed INT8 OSNet embedding vector
};
#pragma pack(pop)

// UI Dimensions
static const int FRAME_W = 256;
static const int FRAME_H = 256;
static const int DISP_W = 512;
static const int DISP_H = 512;
static const int HEADER_H = 36;
static const int SIDEBAR_W = 280;
static const int TOTAL_W = DISP_W + SIDEBAR_W;
static const int TOTAL_H = DISP_H + HEADER_H;

// Distinct Neon/Vivid colors for tracked individuals
static const COLORREF ID_COLORS[] = {
    RGB(0, 255, 255),    // Cyan
    RGB(255, 140, 0),    // Vivid Orange
    RGB(50, 255, 120),   // Neon Green
    RGB(255, 60, 200),   // Vibrant Magenta
    RGB(255, 230, 20),   // Yellow
    RGB(130, 110, 255),  // Purple
    RGB(0, 190, 255),    // Sky Blue
    RGB(255, 80, 80)     // Coral Red
};
static const int NUM_ID_COLORS = sizeof(ID_COLORS) / sizeof(ID_COLORS[0]);

// ReID Profile Structure
struct PersonProfile {
    int id;
    std::vector<float> feature; // L2 normalized feature vector
    COLORREF color;
    uint32_t last_seen_frame;
    int match_count;
    float last_similarity;
    std::chrono::steady_clock::time_point last_seen_time;
};

// Thread Synchronization & Shared Data
static CRITICAL_SECTION g_cs;
static std::vector<uint32_t> g_rgb32_buffer(FRAME_W * FRAME_H, 0); // 32-bit XRGB
static OdMetadataPacket_t g_latest_meta = {};
static uint32_t g_current_frame_id = 0;
static double g_stream_fps = 0.0;
static bool g_has_frame = false;
static bool g_running = true;

// Visual Adjustments
static bool g_swap_rb = false;
static float g_brightness_gain = 1.0f;

// ReID State
static std::vector<PersonProfile> g_gallery;
static int g_latest_reid_id = 0;
static float g_latest_reid_sim = 0.0f;
static COLORREF g_latest_reid_color = RGB(0, 255, 0);
static uint32_t g_latest_reid_frame = 0;
static std::chrono::steady_clock::time_point g_latest_reid_time;
static std::vector<int8_t> g_raw_latest_embedding(REID_EMBEDDING_DIM, 0);
static uint32_t g_reid_total_count = 0;

// Window Handle
static HWND g_hwnd = NULL;

// Helper: Normalize INT8 vector to L2 unit float vector
static std::vector<float> normalize_int8_embedding(const int8_t* raw, size_t len) {
    std::vector<float> vec(len);
    float sum_sq = 0.0f;
    for (size_t i = 0; i < len; i++) {
        vec[i] = static_cast<float>(raw[i]);
        sum_sq += vec[i] * vec[i];
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

            int reid_id = g_latest_reid_id;
            float reid_sim = g_latest_reid_sim;
            COLORREF reid_col = g_latest_reid_color;
            auto reid_time = g_latest_reid_time;
            std::vector<PersonProfile> gallery_copy = g_gallery;
            std::vector<int8_t> spark_emb = g_raw_latest_embedding;
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
                     "  STM32N6 EDGE-AI STREAM | Frame #%u | Video: %.1f FPS | YOLO: %u ms | ReID: ~22 ms | ReID Embs: %u",
                     frame_id, fps, (unsigned int)meta.inference_ms, total_reid);

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

            // 3. Draw Bounding Boxes + ReID Identity Overlay
            auto now = std::chrono::steady_clock::now();
            double reid_age_sec = std::chrono::duration<double>(now - reid_time).count();
            bool reid_active = (reid_id > 0 && reid_age_sec < 2.0);

            for (uint8_t i = 0; i < meta.num_boxes; i++) {
                int cx = (int)(meta.boxes[i].cx * DISP_W);
                int cy = (int)(meta.boxes[i].cy * DISP_H) + HEADER_H;
                int bw = (int)(meta.boxes[i].w * DISP_W);
                int bh = (int)(meta.boxes[i].h * DISP_H);

                int x1 = std::max(0, cx - bw / 2);
                int y1 = std::max(HEADER_H, cy - bh / 2);
                int x2 = std::min(DISP_W - 1, cx + bw / 2);
                int y2 = std::min(HEADER_H + DISP_H - 1, cy + bh / 2);

                COLORREF boxColor = (i == 0 && reid_active) ? reid_col : RGB(0, 255, 120);
                HPEN boxPen = CreatePen(PS_SOLID, 3, boxColor);
                SelectObject(memDC, boxPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));

                Rectangle(memDC, x1, y1, x2, y2);

                // Label Banner above bounding box
                char label[64];
                int conf_pct = (int)(meta.boxes[i].conf * 100.0f);
                if (i == 0 && reid_active) {
                    snprintf(label, sizeof(label), " ID: #%d (%d%%) | Conf: %d%% ",
                             reid_id, (int)(reid_sim * 100.0f), conf_pct);
                } else {
                    snprintf(label, sizeof(label), " Person: %d%% ", conf_pct);
                }

                RECT labelRect = {x1, std::max(HEADER_H, y1 - 22), x1 + 175, y1};
                HBRUSH labelBg = CreateSolidBrush(boxColor);
                FillRect(memDC, &labelRect, labelBg);
                DeleteObject(labelBg);

                SetTextColor(memDC, RGB(0, 0, 0));
                DrawTextA(memDC, label, -1, &labelRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(boxPen);
            }

            // 4. Right Sidebar: ReID Gallery & Telemetry
            RECT sidebarRect = {DISP_W, HEADER_H, TOTAL_W, TOTAL_H};
            HBRUSH sideBrush = CreateSolidBrush(RGB(18, 22, 30));
            FillRect(memDC, &sidebarRect, sideBrush);
            DeleteObject(sideBrush);

            // Vertical divider line between Video and Sidebar
            MoveToEx(memDC, DISP_W, HEADER_H, NULL);
            LineTo(memDC, DISP_W, TOTAL_H);

            // Sidebar Title
            RECT titleRect = {DISP_W + 12, HEADER_H + 10, TOTAL_W - 12, HEADER_H + 32};
            SetTextColor(memDC, RGB(255, 255, 255));
            DrawTextA(memDC, "TARGET RE-IDENTIFICATION", -1, &titleRect, DT_SINGLELINE | DT_LEFT);

            // Subtitle
            RECT subRect = {DISP_W + 12, HEADER_H + 30, TOTAL_W - 12, HEADER_H + 50};
            SetTextColor(memDC, RGB(110, 118, 129));
            DrawTextA(memDC, "Gallery Identities (OSNet INT8)", -1, &subRect, DT_SINGLELINE | DT_LEFT);

            // Divider in sidebar
            MoveToEx(memDC, DISP_W + 10, HEADER_H + 52, NULL);
            LineTo(memDC, TOTAL_W - 10, HEADER_H + 52);

            // Render Gallery List
            int cardY = HEADER_H + 60;
            if (gallery_copy.empty()) {
                RECT emptyRect = {DISP_W + 12, cardY, TOTAL_W - 12, cardY + 30};
                SetTextColor(memDC, RGB(139, 148, 158));
                DrawTextA(memDC, "No targets tracked yet...", -1, &emptyRect, DT_SINGLELINE | DT_LEFT);
            } else {
                for (size_t i = 0; i < gallery_copy.size() && i < 6; i++) {
                    const auto& prof = gallery_copy[i];
                    double age = std::chrono::duration<double>(now - prof.last_seen_time).count();
                    bool active = (age < 2.5);

                    RECT cardRect = {DISP_W + 10, cardY, TOTAL_W - 10, cardY + 44};
                    HBRUSH cardBg = CreateSolidBrush(active ? RGB(26, 35, 48) : RGB(20, 24, 32));
                    FillRect(memDC, &cardRect, cardBg);
                    DeleteObject(cardBg);

                    // Identity Color Badge
                    RECT badgeRect = {DISP_W + 14, cardY + 8, DISP_W + 28, cardY + 36};
                    HBRUSH badgeBrush = CreateSolidBrush(prof.color);
                    FillRect(memDC, &badgeRect, badgeBrush);
                    DeleteObject(badgeBrush);

                    // Text Info
                    char idStr[64];
                    snprintf(idStr, sizeof(idStr), "Person #%d  %s", prof.id, active ? "[LIVE]" : "[LOST]");
                    RECT idRect = {DISP_W + 36, cardY + 4, TOTAL_W - 14, cardY + 22};
                    SetTextColor(memDC, active ? prof.color : RGB(139, 148, 158));
                    DrawTextA(memDC, idStr, -1, &idRect, DT_SINGLELINE | DT_LEFT);

                    char statStr[64];
                    snprintf(statStr, sizeof(statStr), "Matches: %d | Sim: %d%%",
                             prof.match_count, (int)(prof.last_similarity * 100.0f));
                    RECT statRect = {DISP_W + 36, cardY + 22, TOTAL_W - 14, cardY + 40};
                    SetTextColor(memDC, RGB(180, 190, 205));
                    DrawTextA(memDC, statStr, -1, &statRect, DT_SINGLELINE | DT_LEFT);

                    cardY += 50;
                }
            }

            // 5. Live INT8 Embedding Signature (128 Dimensions)
            int sparkY = TOTAL_H - 105;
            RECT sparkTitleRect = {DISP_W + 12, sparkY, TOTAL_W - 12, sparkY + 18};
            SetTextColor(memDC, RGB(0, 240, 255));
            DrawTextA(memDC, "LIVE INT8 EMBEDDING (128-D)", -1, &sparkTitleRect, DT_SINGLELINE | DT_LEFT);

            // Frame box for sparkline
            RECT sparkBox = {DISP_W + 10, sparkY + 20, TOTAL_W - 10, sparkY + 80};
            HBRUSH sparkBg = CreateSolidBrush(RGB(10, 13, 18));
            FillRect(memDC, &sparkBox, sparkBg);
            DeleteObject(sparkBg);

            // Draw center zero line
            int zeroY = sparkY + 50;
            HPEN zeroPen = CreatePen(PS_DOT, 1, RGB(45, 55, 72));
            SelectObject(memDC, zeroPen);
            MoveToEx(memDC, sparkBox.left, zeroY, NULL);
            LineTo(memDC, sparkBox.right, zeroY);
            SelectObject(memDC, oldPen);
            DeleteObject(zeroPen);

            // Draw 128 vertical bars for the embedding
            float barW = (float)(sparkBox.right - sparkBox.left) / 128.0f;
            for (int i = 0; i < REID_EMBEDDING_DIM; i++) {
                int val = spark_emb[i]; // -128 to 127
                int barH = (int)((val / 128.0f) * 26.0f);
                int bx = sparkBox.left + (int)(i * barW);

                COLORREF barColor = (val >= 0) ? RGB(0, 255, 180) : RGB(255, 80, 100);
                HPEN barPen = CreatePen(PS_SOLID, 1, barColor);
                SelectObject(memDC, barPen);
                MoveToEx(memDC, bx, zeroY, NULL);
                LineTo(memDC, bx, zeroY - barH);
                SelectObject(memDC, oldPen);
                DeleteObject(barPen);
            }

            // Bottom controls hint
            RECT hintRect = {DISP_W + 12, TOTAL_H - 20, TOTAL_W - 12, TOTAL_H - 4};
            SetTextColor(memDC, RGB(80, 90, 105));
            DrawTextA(memDC, "[R] Reset | [C] RGB/BGR | [+/-] Gain | [Q] Quit", -1, &hintRect, DT_SINGLELINE | DT_LEFT);

            // Composite blit to screen
            BitBlt(hdc, 0, 0, TOTAL_W, TOTAL_H, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldPen);
            DeleteObject(divPen);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY: {
            g_running = false;
            PostQuitMessage(0);
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE || wParam == 'Q') {
                DestroyWindow(hwnd);
            } else if (wParam == 'R') {
                // Reset ReID Gallery
                EnterCriticalSection(&g_cs);
                g_gallery.clear();
                g_latest_reid_id = 0;
                g_latest_reid_sim = 0.0f;
                LeaveCriticalSection(&g_cs);
                std::cout << "[*] ReID Identity Gallery Reset.\n";
            } else if (wParam == 'C') {
                g_swap_rb = !g_swap_rb;
                std::cout << "[*] Color Mode: " << (g_swap_rb ? "BGR" : "RGB") << "\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == VK_ADD || wParam == VK_OEM_PLUS) {
                g_brightness_gain = std::min(10.0f, g_brightness_gain + 0.5f);
                std::cout << "[*] Brightness Gain: " << g_brightness_gain << "x\n";
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == VK_SUBTRACT || wParam == VK_OEM_MINUS) {
                g_brightness_gain = std::max(0.5f, g_brightness_gain - 0.5f);
                std::cout << "[*] Brightness Gain: " << g_brightness_gain << "x\n";
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Dedicated UDP Networking Receiver Thread
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
        return 1;
    }

    // Set 4MB Socket receive buffer
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(LISTEN_PORT);

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "[!] Bind failed on port " << LISTEN_PORT << ". Is another process using it?\n";
        closesocket(sock);
        return 1;
    }

    std::cout << "[+] C++ UDP Server listening on port " << LISTEN_PORT << "...\n";

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
        if (pkt_type == PKT_TYPE_REID_METADATA && bytes >= (int)sizeof(ReidMetadataPacket_t)) {
            ReidMetadataPacket_t reid_pkt;
            memcpy(&reid_pkt, recv_buf.data(), sizeof(ReidMetadataPacket_t));

            std::vector<float> norm_emb = normalize_int8_embedding(reid_pkt.embedding, REID_EMBEDDING_DIM);

            EnterCriticalSection(&g_cs);
            int best_id = -1;
            float best_sim = -1.0f;
            int best_idx = -1;

            for (size_t i = 0; i < g_gallery.size(); i++) {
                float sim = compute_cosine_similarity(g_gallery[i].feature, norm_emb);
                if (sim > best_sim) {
                    best_sim = sim;
                    best_id = g_gallery[i].id;
                    best_idx = (int)i;
                }
            }

            if (best_sim >= REID_SIMILARITY_THRESHOLD && best_idx >= 0) {
                // Update existing person profile with EMA
                for (size_t i = 0; i < REID_EMBEDDING_DIM; i++) {
                    g_gallery[best_idx].feature[i] = 0.80f * g_gallery[best_idx].feature[i] + 0.20f * norm_emb[i];
                }
                // Re-normalize
                float s_sq = 0.0f;
                for (float v : g_gallery[best_idx].feature) s_sq += v * v;
                float n_val = std::sqrt(s_sq);
                if (n_val > 1e-6f) {
                    for (float &v : g_gallery[best_idx].feature) v /= n_val;
                }
                g_gallery[best_idx].match_count++;
                g_gallery[best_idx].last_seen_frame = reid_pkt.frame_id;
                g_gallery[best_idx].last_similarity = best_sim;
                g_gallery[best_idx].last_seen_time = std::chrono::steady_clock::now();

                g_latest_reid_id = best_id;
                g_latest_reid_sim = best_sim;
                g_latest_reid_color = g_gallery[best_idx].color;
            } else {
                // Register new unique identity
                int new_id = (int)g_gallery.size() + 1;
                COLORREF color = ID_COLORS[(new_id - 1) % NUM_ID_COLORS];
                PersonProfile p;
                p.id = new_id;
                p.feature = norm_emb;
                p.color = color;
                p.last_seen_frame = reid_pkt.frame_id;
                p.match_count = 1;
                p.last_similarity = 1.0f;
                p.last_seen_time = std::chrono::steady_clock::now();
                g_gallery.push_back(p);

                g_latest_reid_id = new_id;
                g_latest_reid_sim = 1.0f;
                g_latest_reid_color = color;

                std::cout << "[+] Registered New Identity: Person #" << new_id << "\n";
            }

            g_latest_reid_frame = reid_pkt.frame_id;
            g_latest_reid_time = std::chrono::steady_clock::now();
            g_raw_latest_embedding.assign(reid_pkt.embedding, reid_pkt.embedding + REID_EMBEDDING_DIM);
            g_reid_total_count++;
            LeaveCriticalSection(&g_cs);

            if (g_hwnd) {
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        }
        // 2. OD Metadata Packet
        else if (pkt_type == PKT_TYPE_OD_METADATA && bytes >= (int)sizeof(OdMetadataPacket_t)) {
            memcpy(&current_meta, recv_buf.data(), sizeof(OdMetadataPacket_t));
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
                    bool swap = g_swap_rb;
                    float gain = g_brightness_gain;
                    for (int i = 0; i < FRAME_W * FRAME_H; i++) {
                        uint16_t c = p565[i];
                        uint8_t cr = (c >> 11) & 0x1F;
                        uint8_t cg = (c >> 5) & 0x3F;
                        uint8_t cb = c & 0x1F;
                        uint8_t r = (cr * 527 + 23) >> 6;
                        uint8_t g = (cg * 259 + 33) >> 6;
                        uint8_t b = (cb * 527 + 23) >> 6;

                        if (swap) {
                            std::swap(r, b);
                        }
                        if (gain != 1.0f) {
                            int ir = (int)(r * gain);
                            int ig = (int)(g * gain);
                            int ib = (int)(b * gain);
                            r = (uint8_t)std::min(255, ir);
                            g = (uint8_t)std::min(255, ig);
                            b = (uint8_t)std::min(255, ib);
                        }
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

    std::cout << "=========================================================\n";
    std::cout << "  STM32N6 Object Re-Identification (ReID) Viewer Server  \n";
    std::cout << "=========================================================\n";
    std::cout << "[*] Starting background multi-threaded UDP receiver...\n";

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

    // Window size: 792x548 (512x512 Video + 280px Gallery Sidebar + 36px Top Banner)
    RECT wr = {0, 0, TOTAL_W, TOTAL_H};
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwnd = CreateWindowExA(
        0,
        "STM32N6_ReID_Viewer_Class",
        "STM32N6 Object Detection & Re-Identification HUD",
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
    std::cout << "    [ESC/Q] Exit\n";

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
