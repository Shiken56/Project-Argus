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

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define STREAM_MAGIC 0x54524F4EU
#define PKT_TYPE_VIDEO_CHUNK 0x01
#define PKT_TYPE_OD_METADATA 0x02
#define LISTEN_PORT 5000
#define OD_MAX_BOXES 10

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
#pragma pack(pop)

// Shared state between receiver thread and GUI
static const int FRAME_W = 256;
static const int FRAME_H = 256;
static const int DISP_W = 512;
static const int DISP_H = 512;

static CRITICAL_SECTION g_cs;
static std::vector<uint32_t> g_rgb32_buffer(FRAME_W * FRAME_H, 0); // 32-bit XRGB for GDI
static OdMetadataPacket_t g_latest_meta = {};
static uint32_t g_current_frame_id = 0;
static double g_stream_fps = 0.0;
static bool g_has_frame = false;
static bool g_running = true;

// Window Handle
static HWND g_hwnd = NULL;

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
            LeaveCriticalSection(&g_cs);

            // Double buffering memory DC
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, DISP_W, DISP_H + 32);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            // Create DIB section to blit our 256x256 image
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = FRAME_W;
            bmi.bmiHeader.biHeight = -FRAME_H; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            // Header status bar
            RECT bannerRect = {0, 0, DISP_W, 32};
            HBRUSH bannerBrush = CreateSolidBrush(RGB(20, 20, 25));
            FillRect(memDC, &bannerRect, bannerBrush);
            DeleteObject(bannerBrush);

            char bannerText[128];
            snprintf(bannerText, sizeof(bannerText),
                     " Frame #%u | NPU: %u ms | Stream: %.1f FPS | Detections: %u",
                     frame_id, (unsigned int)meta.inference_ms, fps, (unsigned int)meta.num_boxes);

            SetTextColor(memDC, RGB(0, 255, 255));
            SetBkMode(memDC, TRANSPARENT);
            DrawTextA(memDC, bannerText, -1, &bannerRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

            // StretchBlt from 256x256 to 512x512
            SetStretchBltMode(memDC, COLORONCOLOR);
            StretchDIBits(memDC,
                          0, 32, DISP_W, DISP_H,
                          0, 0, FRAME_W, FRAME_H,
                          local_pixels.data(),
                          &bmi,
                          DIB_RGB_COLORS,
                          SRCCOPY);

            // Draw bounding boxes on top
            HPEN boxPen = CreatePen(PS_SOLID, 3, RGB(0, 255, 0));
            HPEN oldPen = (HPEN)SelectObject(memDC, boxPen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(HOLLOW_BRUSH));

            for (uint8_t i = 0; i < meta.num_boxes; i++) {
                int cx = (int)(meta.boxes[i].cx * DISP_W);
                int cy = (int)(meta.boxes[i].cy * DISP_H) + 32;
                int bw = (int)(meta.boxes[i].w * DISP_W);
                int bh = (int)(meta.boxes[i].h * DISP_H);

                int x1 = std::max(0, cx - bw / 2);
                int y1 = std::max(32, cy - bh / 2);
                int x2 = std::min(DISP_W - 1, cx + bw / 2);
                int y2 = std::min(DISP_H + 32 - 1, cy + bh / 2);

                Rectangle(memDC, x1, y1, x2, y2);

                char label[32];
                int conf_pct = (int)(meta.boxes[i].conf * 100.0f);
                snprintf(label, sizeof(label), " Obj: %d%% ", conf_pct);

                RECT labelRect = {x1, std::max(32, y1 - 20), x1 + 80, y1};
                HBRUSH labelBg = CreateSolidBrush(RGB(0, 255, 0));
                FillRect(memDC, &labelRect, labelBg);
                DeleteObject(labelBg);

                SetTextColor(memDC, RGB(0, 0, 0));
                DrawTextA(memDC, label, -1, &labelRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            }

            SelectObject(memDC, oldPen);
            SelectObject(memDC, oldBrush);
            DeleteObject(boxPen);

            // Blit composite buffer to screen
            BitBlt(hdc, 0, 0, DISP_W, DISP_H + 32, memDC, 0, 0, SRCCOPY);

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

        // Metadata packet
        if (pkt_type == PKT_TYPE_OD_METADATA && bytes >= (int)sizeof(OdMetadataPacket_t)) {
            memcpy(&current_meta, recv_buf.data(), sizeof(OdMetadataPacket_t));
        }
        // Video chunk packet
        else if (pkt_type == PKT_TYPE_VIDEO_CHUNK && bytes >= (int)sizeof(VideoChunkHeader_t)) {
            VideoChunkHeader_t hdr;
            memcpy(&hdr, recv_buf.data(), sizeof(VideoChunkHeader_t));

            int payload_len = hdr.payload_len;
            if (sizeof(VideoChunkHeader_t) + payload_len <= (size_t)bytes) {
                // Auto stride: 1400 bytes for RGB565 (<= 100 chunks), 1024 for RGB888
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

                // Frame complete or last chunk
                if (chunks_received >= hdr.total_chunks || hdr.chunk_idx == hdr.total_chunks - 1) {
                    // Fast convert RGB565 to 32-bit XRGB for high-speed Direct GDI blitting
                    EnterCriticalSection(&g_cs);
                    const uint16_t* p565 = (const uint16_t*)raw_frame_565.data();
                    for (int i = 0; i < FRAME_W * FRAME_H; i++) {
                        uint16_t c = p565[i];
                        uint8_t r = (c >> 11) & 0x1F;
                        uint8_t g = (c >> 5) & 0x3F;
                        uint8_t b = c & 0x1F;
                        // Scale 5/6 bits to 8 bits
                        r = (r * 527 + 23) >> 6;
                        g = (g * 259 + 33) >> 6;
                        b = (b * 527 + 23) >> 6;
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

                    // Trigger Window Repaint
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
    std::cout << "  STM32N6 Native High-Speed C++ Video Stream Viewer      \n";
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
    wc.lpszClassName = "STM32N6_Viewer_Class";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    if (!RegisterClassExA(&wc)) {
        std::cerr << "[!] RegisterClassEx failed\n";
        return 1;
    }

    // Window size: 512x544 (512x512 video + 32px top status banner)
    RECT wr = {0, 0, DISP_W, DISP_H + 32};
    AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwnd = CreateWindowExA(
        0,
        "STM32N6_Viewer_Class",
        "STM32N6 Object Detection Live Stream (C++ Native)",
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

    std::cout << "[+] Window created successfully. Press 'q' or 'ESC' to exit.\n";

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
