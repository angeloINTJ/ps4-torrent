// =============================================================================
// ui/renderer.cpp — PS4 homebrew UI implementation
//
// Combines:
//   - 1080p SceVideoOut direct framebuffer rendering (with built-in 8x8 font)
//   - Native PS4 toast notifications (sceKernelSendNotificationRequest)
//   - Controller input via SceUserService + ScePad
//   - Debug log output via printf
// =============================================================================

#include "ui/renderer.hpp"

// PS4 headers via OpenOrbis
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#define PS4_LOG(...) printf(__VA_ARGS__)

namespace bt {
namespace ui {

namespace {

// =============================================================================
// System & VideoOut state
// =============================================================================

constexpr int FB_WIDTH  = 1920;
constexpr int FB_HEIGHT = 1080;
constexpr int FB_DEPTH  = 4;
constexpr int FB_COUNT  = 2;
constexpr size_t FB_BYTES = static_cast<size_t>(FB_WIDTH) * FB_HEIGHT * FB_DEPTH;
constexpr size_t DMEM_SIZE  = 0xC000000; // 192 MiB direct memory pool
constexpr size_t DMEM_ALIGN = 0x200000;  // 2 MiB alignment

int32_t           g_user_id       = -1;
int32_t           g_pad_handle    = -1;
bool              g_services_ok   = false;

int32_t           g_video_handle  = -1;
OrbisKernelEqueue g_flip_queue    = 0;
off_t             g_dmem_offset   = 0;
void*             g_video_mem     = nullptr;
uint32_t*         g_framebuffers[FB_COUNT] = {nullptr, nullptr};
int               g_active_fb     = 0;
int64_t           g_frame_id      = 1;
bool              g_video_ok      = false;

std::mutex        g_render_mutex;
int               g_last_notified_milestone = -1;

// =============================================================================
// Built-in 8x8 ASCII bitmap font (printable chars 0x20..0x7E)
// Each glyph is 8 rows of 1 byte (MSB = leftmost pixel)
// =============================================================================

constexpr uint8_t FONT_8X8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, // '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5'
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, // '='
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // '>'
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // '?'
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, // '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'A'
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 'B'
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 'C'
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 'D'
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 'E'
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 'F'
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, // 'G'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 'J'
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 'K'
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 'L'
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 'M'
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'N'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'O'
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 'P'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0E}, // 'Q'
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 'R'
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 'S'
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'T'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'U'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'V'
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, // 'W'
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 'Y'
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, // 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 'a'
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, // 'b'
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 'e'
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, // 'f'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, // 'g'
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, // 'j'
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, // 'm'
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 'p'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 'q'
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 'r'
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 's'
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, // 't'
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'v'
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, // 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 'x'
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, // 'y'
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};

constexpr uint32_t make_color(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0x80000000u | (static_cast<uint32_t>(r) << 16)
                       | (static_cast<uint32_t>(g) << 8)
                       | static_cast<uint32_t>(b);
}

constexpr uint32_t COL_BG         = make_color(18,  20,  34);   // #121422
constexpr uint32_t COL_CARD       = make_color(30,  34,  56);   // #1e2238
constexpr uint32_t COL_HEADER     = make_color(233, 69,  96);   // #e94560
constexpr uint32_t COL_TEXT       = make_color(240, 242, 248);  // #f0f2f8
constexpr uint32_t COL_SUBTEXT    = make_color(160, 170, 195);  // #a0aac3
constexpr uint32_t COL_BAR_BG     = make_color(15,  17,  28);   // #0f111c
constexpr uint32_t COL_BAR_FILL   = make_color(46,  204, 113);  // #2ecc71
constexpr uint32_t COL_ERROR      = make_color(231, 76,  60);   // #e74c3c

void fb_fill_rect(uint32_t* fb, int rx, int ry, int rw, int rh, uint32_t color) {
    int x0 = std::max(0, rx);
    int y0 = std::max(0, ry);
    int x1 = std::min(FB_WIDTH,  rx + rw);
    int y1 = std::min(FB_HEIGHT, ry + rh);

    for (int y = y0; y < y1; ++y) {
        uint32_t* row = fb + y * FB_WIDTH;
        for (int x = x0; x < x1; ++x) {
            row[x] = color;
        }
    }
}

void fb_draw_char(uint32_t* fb, int cx, int cy, char c, uint32_t color, int scale) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = FONT_8X8[c - 0x20];

    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80u >> col)) {
                fb_fill_rect(fb, cx + col * scale, cy + row * scale, scale, scale, color);
            }
        }
    }
}

void fb_draw_text(uint32_t* fb, int x, int y, const std::string& text, uint32_t color, int scale = 3) {
    int cur_x = x;
    int cur_y = y;
    int advance = 8 * scale + scale;
    int line_h  = 10 * scale;

    for (char c : text) {
        if (c == '\n') {
            cur_x = x;
            cur_y += line_h;
            continue;
        }
        if (cur_x + 8 * scale < FB_WIDTH - 80) {
            fb_draw_char(fb, cur_x, cur_y, c, color, scale);
        }
        cur_x += advance;
    }
}

void fb_present() {
    if (!g_video_ok) return;

    sceVideoOutSubmitFlip(g_video_handle, g_active_fb, ORBIS_VIDEO_OUT_FLIP_VSYNC, g_frame_id);

    OrbisVideoOutFlipStatus status{};
    for (int tries = 0; tries < 10; ++tries) {
        sceVideoOutGetFlipStatus(g_video_handle, &status);
        if (status.flipArg >= g_frame_id) break;
        OrbisKernelEvent evt{};
        int count = 0;
        sceKernelWaitEqueue(g_flip_queue, &evt, 1, &count, nullptr);
    }

    ++g_frame_id;
    g_active_fb = (g_active_fb + 1) % FB_COUNT;
}

void fb_draw_base_card(uint32_t* fb, uint32_t accent_color) {
    fb_fill_rect(fb, 0, 0, FB_WIDTH, FB_HEIGHT, COL_BG);
    // Card container
    fb_fill_rect(fb, 160, 120, FB_WIDTH - 320, FB_HEIGHT - 240, COL_CARD);
    // Top accent bar
    fb_fill_rect(fb, 160, 120, FB_WIDTH - 320, 84, accent_color);
    fb_draw_text(fb, 200, 148, "PS4 TORRENT v0.2  (FW <= 14.02)", COL_TEXT, 3);
}

bool init_video_out() {
    g_video_handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (g_video_handle < 0) {
        PS4_LOG("[ui] sceVideoOutOpen failed: 0x%08X\n", g_video_handle);
        return false;
    }

    if (sceKernelCreateEqueue(&g_flip_queue, "ps4_torrent_flip") < 0) {
        PS4_LOG("[ui] sceKernelCreateEqueue failed\n");
        sceVideoOutClose(g_video_handle);
        g_video_handle = -1;
        return false;
    }
    sceVideoOutAddFlipEvent(g_flip_queue, g_video_handle, nullptr);

    int32_t rc = sceKernelAllocateDirectMemory(
        0, sceKernelGetDirectMemorySize(), DMEM_SIZE, DMEM_ALIGN, 3, &g_dmem_offset);
    if (rc < 0) {
        PS4_LOG("[ui] sceKernelAllocateDirectMemory failed: 0x%08X\n", rc);
        sceVideoOutClose(g_video_handle);
        g_video_handle = -1;
        return false;
    }

    rc = sceKernelMapDirectMemory(
        &g_video_mem, DMEM_SIZE, 0x33, 0, g_dmem_offset, DMEM_ALIGN);
    if (rc < 0 || !g_video_mem) {
        PS4_LOG("[ui] sceKernelMapDirectMemory failed: 0x%08X\n", rc);
        sceKernelReleaseDirectMemory(g_dmem_offset, DMEM_SIZE);
        sceVideoOutClose(g_video_handle);
        g_video_handle = -1;
        return false;
    }

    uint8_t* base = static_cast<uint8_t*>(g_video_mem);
    for (int i = 0; i < FB_COUNT; ++i) {
        g_framebuffers[i] = reinterpret_cast<uint32_t*>(base + i * FB_BYTES);
        fb_fill_rect(g_framebuffers[i], 0, 0, FB_WIDTH, FB_HEIGHT, COL_BG);
    }

    OrbisVideoOutBufferAttribute attr{};
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, FB_WIDTH, FB_HEIGHT, FB_WIDTH);

    rc = sceVideoOutRegisterBuffers(
        g_video_handle, 0, reinterpret_cast<void* const*>(g_framebuffers), FB_COUNT, &attr);
    if (rc < 0) {
        PS4_LOG("[ui] sceVideoOutRegisterBuffers failed: 0x%08X\n", rc);
        sceKernelReleaseDirectMemory(g_dmem_offset, DMEM_SIZE);
        sceVideoOutClose(g_video_handle);
        g_video_handle = -1;
        return false;
    }

    sceVideoOutSetFlipRate(g_video_handle, 0);
    g_video_ok = true;
    return true;
}

} // anonymous namespace

// =============================================================================
// notify — PS4 system pop-up notification
// =============================================================================

void notify(const std::string& message) {
    OrbisNotificationRequest req{};
    req.type             = NotificationRequest;
    req.reqId            = 0;
    req.priority         = 0;
    req.msgId            = 0;
    req.targetId         = -1;
    req.userId           = -1;
    req.useIconImageUri  = 1;

    std::snprintf(req.iconUri, sizeof(req.iconUri),
                  "cxml://psnotification/tex_icon_system");
    std::snprintf(req.message, sizeof(req.message),
                  "PS4 Torrent: %s", message.c_str());

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// =============================================================================
// init_system_services / shutdown_system_services
// =============================================================================

void init_system_services() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Initialize video out and hide the OS splash screen so the app never hangs on boot
    if (init_video_out()) {
        sceSystemServiceHideSplashScreen();
        render_status("Initializing PS4 Torrent...", "Setting up controller and storage directories...");
    } else {
        sceSystemServiceHideSplashScreen();
    }

    // Initialize Pad library
    int32_t ret = scePadInit();
    if (ret != 0) {
        PS4_LOG("[ui] scePadInit failed: 0x%08X\n", ret);
    }

    // Initialize UserService with proper FIFO priority (required across FW 5.05–11.02)
    OrbisUserServiceInitializeParams param{};
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    ret = sceUserServiceInitialize(&param);
    if (ret != 0) {
        PS4_LOG("[ui] sceUserServiceInitialize returned: 0x%08X\n", ret);
    }

    ret = sceUserServiceGetInitialUser(&g_user_id);
    if (ret != 0 || g_user_id < 0) {
        PS4_LOG("[ui] sceUserServiceGetInitialUser failed: 0x%08X\n", ret);
        return;
    }

    g_pad_handle = scePadOpen(g_user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (g_pad_handle < 0) {
        PS4_LOG("[ui] scePadOpen failed: 0x%08X\n", g_pad_handle);
        return;
    }

    g_services_ok = true;
    PS4_LOG("[ui] System services initialized. UserID=%d PadHandle=%d VideoOut=%d\n",
            g_user_id, g_pad_handle, g_video_handle);
}

void shutdown_system_services() {
    std::lock_guard<std::mutex> lk(g_render_mutex);
    if (g_pad_handle >= 0) {
        scePadClose(g_pad_handle);
        g_pad_handle = -1;
    }
    if (g_video_ok) {
        g_video_ok = false;
        if (g_video_handle >= 0) {
            sceVideoOutClose(g_video_handle);
            g_video_handle = -1;
        }
        if (g_dmem_offset != 0) {
            sceKernelReleaseDirectMemory(g_dmem_offset, DMEM_SIZE);
            g_dmem_offset = 0;
        }
    }
}

// =============================================================================
// Controller input checks
// =============================================================================

bool check_cancel_button() {
    if (!g_services_ok || g_pad_handle < 0) return false;

    OrbisPadData pad_data{};
    if (scePadReadState(g_pad_handle, &pad_data) != 0) return false;

    return (pad_data.buttons & ORBIS_PAD_BUTTON_CIRCLE) != 0;
}

bool check_confirm_button() {
    if (!g_services_ok || g_pad_handle < 0) return false;

    OrbisPadData pad_data{};
    if (scePadReadState(g_pad_handle, &pad_data) != 0) return false;

    return (pad_data.buttons & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_OPTIONS)) != 0;
}

// =============================================================================
// Formatting
// =============================================================================

std::string format_bytes(int64_t bytes) {
    char buf[32];
    if (bytes < 1024LL) {
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    } else if (bytes < 1024LL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

std::string format_speed(double bps) {
    return format_bytes(static_cast<int64_t>(bps)) + "/s";
}

std::string progress_bar(float pct, int width) {
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    int filled = static_cast<int>(std::round(pct / 100.0f * width));

    std::string bar = "[";
    for (int i = 0; i < width; ++i) {
        bar += (i < filled) ? '#' : '.';
    }
    bar += ']';
    return bar;
}

// =============================================================================
// render_status
// =============================================================================

void render_status(const std::string& title, const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_render_mutex);

    PS4_LOG("[ui] %s -- %s\n", title.c_str(), detail.c_str());

    if (g_video_ok) {
        uint32_t* fb = g_framebuffers[g_active_fb];
        fb_draw_base_card(fb, COL_HEADER);
        fb_draw_text(fb, 220, 280, title,  COL_TEXT,    3);
        fb_draw_text(fb, 220, 360, detail, COL_SUBTEXT, 2);
        fb_present();
    }
}

// =============================================================================
// render_progress
// =============================================================================

void render_progress(
    const std::string&  torrent_name,
    int64_t             total_size,
    const ProgressInfo& info)
{
    std::lock_guard<std::mutex> lk(g_render_mutex);

    PS4_LOG("\n============================================================\n");
    PS4_LOG("  PS4 Torrent v0.2\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  File      : %s\n", torrent_name.c_str());
    PS4_LOG("  Size      : %s\n", format_bytes(total_size).c_str());
    PS4_LOG("  %s %.1f%%\n", progress_bar(info.pct).c_str(), info.pct);
    PS4_LOG("  Downloaded: %s / %s\n",
            format_bytes(info.bytes_done).c_str(),
            format_bytes(info.bytes_total).c_str());
    PS4_LOG("  Speed     : %s\n", format_speed(info.speed_bps).c_str());
    PS4_LOG("  Peers     : %d  Seeders: %d  Leechers: %d\n",
            info.peers_active, info.seeders, info.leechers);
    PS4_LOG("  [O] Cancel\n");
    PS4_LOG("============================================================\n");

    // Emit native PS4 toast notifications at 25%, 50%, 75%
    int milestone = static_cast<int>(info.pct) / 25 * 25;
    if (milestone >= 25 && milestone < 100 && milestone > g_last_notified_milestone) {
        g_last_notified_milestone = milestone;
        char nbuf[256];
        std::snprintf(nbuf, sizeof(nbuf), "%s (%d%% - %s)",
                      torrent_name.c_str(), milestone, format_speed(info.speed_bps).c_str());
        notify(nbuf);
    }

    if (g_video_ok) {
        uint32_t* fb = g_framebuffers[g_active_fb];
        fb_draw_base_card(fb, COL_HEADER);

        fb_draw_text(fb, 220, 250, "File : " + torrent_name, COL_TEXT, 3);
        fb_draw_text(fb, 220, 310, "Size : " + format_bytes(total_size), COL_SUBTEXT, 3);

        // Graphical progress bar
        int bar_x = 220;
        int bar_y = 400;
        int bar_w = FB_WIDTH - 440;
        int bar_h = 48;
        fb_fill_rect(fb, bar_x, bar_y, bar_w, bar_h, COL_BAR_BG);

        float clamped_pct = std::max(0.0f, std::min(100.0f, info.pct));
        int fill_w = static_cast<int>((clamped_pct / 100.0f) * bar_w);
        if (fill_w > 0) {
            fb_fill_rect(fb, bar_x, bar_y, fill_w, bar_h, COL_BAR_FILL);
        }

        char pct_buf[128];
        std::snprintf(pct_buf, sizeof(pct_buf), "%.1f%%   (%s / %s)",
                      info.pct,
                      format_bytes(info.bytes_done).c_str(),
                      format_bytes(info.bytes_total).c_str());
        fb_draw_text(fb, 220, 475, pct_buf, COL_TEXT, 3);

        fb_draw_text(fb, 220, 555, "Speed : " + format_speed(info.speed_bps), COL_TEXT, 3);

        char peers_buf[128];
        std::snprintf(peers_buf, sizeof(peers_buf),
                      "Peers : %d active   |   Seeders: %d   |   Leechers: %d",
                      info.peers_active, info.seeders, info.leechers);
        fb_draw_text(fb, 220, 620, peers_buf, COL_SUBTEXT, 3);

        fb_draw_text(fb, 220, 860, "Press [O] Circle to cancel download", COL_SUBTEXT, 2);

        fb_present();
    }
}

// =============================================================================
// render_error
// =============================================================================

void render_error(const std::string& message) {
    std::lock_guard<std::mutex> lk(g_render_mutex);

    PS4_LOG("\n============================================================\n");
    PS4_LOG("  PS4 Torrent v0.2  --  ERROR\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  %s\n", message.c_str());
    PS4_LOG("============================================================\n");

    notify("Error - check screen for details");

    if (g_video_ok) {
        uint32_t* fb = g_framebuffers[g_active_fb];
        fb_draw_base_card(fb, COL_ERROR);
        fb_draw_text(fb, 220, 260, "ERROR", COL_ERROR, 4);
        fb_draw_text(fb, 220, 350, message, COL_TEXT, 2);
        fb_draw_text(fb, 220, 860, "Press [X] Cross or [O] Circle to exit", COL_SUBTEXT, 2);
        fb_present();
    }
}

// =============================================================================
// render_complete
// =============================================================================

void render_complete(
    const std::string& torrent_name,
    int64_t            total_bytes,
    double             elapsed_sec,
    const std::string& save_dir)
{
    std::lock_guard<std::mutex> lk(g_render_mutex);

    double avg_speed = (elapsed_sec > 0) ? (total_bytes / elapsed_sec) : 0.0;

    PS4_LOG("\n============================================================\n");
    PS4_LOG("  PS4 Torrent v0.2  --  COMPLETE!\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  File     : %s\n", torrent_name.c_str());
    PS4_LOG("  Total    : %s\n", format_bytes(total_bytes).c_str());
    PS4_LOG("  Time     : %.0f seconds\n", elapsed_sec);
    PS4_LOG("  Avg speed: %s\n", format_speed(avg_speed).c_str());
    PS4_LOG("  Saved to : %s\n", save_dir.c_str());
    PS4_LOG("  Press [X] to exit.\n");
    PS4_LOG("============================================================\n");

    notify("Download complete: " + torrent_name);

    if (g_video_ok) {
        uint32_t* fb = g_framebuffers[g_active_fb];
        fb_draw_base_card(fb, COL_BAR_FILL);

        fb_draw_text(fb, 220, 260, "DOWNLOAD COMPLETE!", COL_BAR_FILL, 4);
        fb_draw_text(fb, 220, 360, "File      : " + torrent_name, COL_TEXT, 3);
        fb_draw_text(fb, 220, 425, "Total     : " + format_bytes(total_bytes), COL_TEXT, 3);

        char time_buf[64];
        std::snprintf(time_buf, sizeof(time_buf), "Time      : %.0f seconds", elapsed_sec);
        fb_draw_text(fb, 220, 490, time_buf, COL_SUBTEXT, 3);
        fb_draw_text(fb, 220, 555, "Avg Speed : " + format_speed(avg_speed), COL_SUBTEXT, 3);
        fb_draw_text(fb, 220, 640, "Saved to  : " + save_dir, COL_TEXT, 3);
        fb_draw_text(fb, 220, 860, "Press [X] Cross or [O] Circle to exit", COL_SUBTEXT, 2);

        fb_present();
    }
}

} // namespace ui
} // namespace bt
