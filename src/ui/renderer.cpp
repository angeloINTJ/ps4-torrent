// =============================================================================
// ui/renderer.cpp — PS4 homebrew UI implementation
//
// Uses sceUserService and scePad for input, and OrbisKernelDebugOutA
// (mapped via an OpenOrbis include) for debug log output.
// =============================================================================

#include "ui/renderer.hpp"

// PS4 headers via OpenOrbis
// These files exist at $OO_PS4_TOOLCHAIN/include/
#include <orbis/UserService.h>
#include <orbis/Pad.h>

#include <cstdio>
#include <cstring>
#include <cmath>

// In OpenOrbis, printf() writes to OrbisKernelDebugOutA automatically
// via musl + libkernel hooks when running with ps4debug active.
#define PS4_LOG(...) printf(__VA_ARGS__)

namespace bt {
namespace ui {

// =============================================================================
// Input system state
// =============================================================================

static int32_t  g_user_id    = -1;
static int32_t  g_pad_handle = -1;
static bool     g_services_ok = false;

// =============================================================================
// init_system_services
// =============================================================================

void init_system_services() {
    // Initialize UserService (required to get the userId for Pad)
    int32_t ret = sceUserServiceInitialize(nullptr);
    if (ret != 0) {
        PS4_LOG("[ui] sceUserServiceInitialize failed: 0x%08X\n", ret);
        return;
    }

    // Get the initial user ID (first logged-in user)
    ret = sceUserServiceGetInitialUser(&g_user_id);
    if (ret != 0) {
        PS4_LOG("[ui] sceUserServiceGetInitialUser failed: 0x%08X\n", ret);
        return;
    }

    // Initialize the Pad (controller) module
    ret = scePadInit();
    if (ret != 0) {
        PS4_LOG("[ui] scePadInit failed: 0x%08X\n", ret);
        return;
    }

    // Open the user's controller (type 0 = DualShock 4)
    g_pad_handle = scePadOpen(g_user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (g_pad_handle < 0) {
        PS4_LOG("[ui] scePadOpen failed: 0x%08X\n", g_pad_handle);
        return;
    }

    g_services_ok = true;
    PS4_LOG("[ui] System services initialized. UserID=%d PadHandle=%d\n",
            g_user_id, g_pad_handle);
}

// =============================================================================
// check_cancel_button — checks Circle (O) button press
// =============================================================================

bool check_cancel_button() {
    if (!g_services_ok || g_pad_handle < 0) return false;

    OrbisPadData pad_data{};
    int32_t ret = scePadReadState(g_pad_handle, &pad_data);
    if (ret != 0) return false;

    // ORBIS_PAD_BUTTON_CIRCLE = Circle button (cancel/back on JP/BR layout)
    return (pad_data.buttons & ORBIS_PAD_BUTTON_CIRCLE) != 0;
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
// render_progress
// =============================================================================

void render_progress(
    const std::string& torrent_name,
    int64_t            total_size,
    const ProgressInfo& info)
{
    // Clear the debug screen by emitting several blank lines
    // (UART/ps4debug has no guaranteed terminal escape codes)
    PS4_LOG("\n\n\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  PS4 Torrent v0.1\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  File     : %s\n", torrent_name.c_str());
    PS4_LOG("  Size     : %s\n", format_bytes(total_size).c_str());
    PS4_LOG("\n");
    PS4_LOG("  %s %.1f%%\n", progress_bar(info.pct).c_str(), info.pct);
    PS4_LOG("  Downloaded: %s / %s\n",
            format_bytes(info.bytes_done).c_str(),
            format_bytes(info.bytes_total).c_str());
    PS4_LOG("  Speed    : %s\n", format_speed(info.speed_bps).c_str());
    PS4_LOG("  Peers    : %d  Seeders: %d  Leechers: %d\n",
            info.peers_active, info.seeders, info.leechers);
    PS4_LOG("\n");
    PS4_LOG("  [O] Cancel\n");
    PS4_LOG("============================================================\n");
}

// =============================================================================
// render_error
// =============================================================================

void render_error(const std::string& message) {
    PS4_LOG("\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  PS4 Torrent v0.1  --  ERROR\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  %s\n", message.c_str());
    PS4_LOG("============================================================\n");
}

// =============================================================================
// render_complete
// =============================================================================

void render_complete(const std::string& torrent_name, int64_t total_bytes, double elapsed_sec) {
    double avg_speed = (elapsed_sec > 0) ? (total_bytes / elapsed_sec) : 0.0;

    PS4_LOG("\n\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  PS4 Torrent v0.1  --  COMPLETE!\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  File     : %s\n", torrent_name.c_str());
    PS4_LOG("  Total    : %s\n", format_bytes(total_bytes).c_str());
    PS4_LOG("  Time     : %.0f seconds\n", elapsed_sec);
    PS4_LOG("  Avg speed: %s\n", format_speed(avg_speed).c_str());
    PS4_LOG("\n");
    PS4_LOG("  Press [X] to exit.\n");
    PS4_LOG("============================================================\n");
}

} // namespace ui
} // namespace bt
