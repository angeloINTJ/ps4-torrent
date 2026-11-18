// =============================================================================
// ui/renderer.hpp — UI layer for PS4 homebrew (Framebuffer + Notifications + Log)
//
// Provides three simultaneous output channels on the PS4 (FW ≤ 11.02+):
//   1. 1080p TV Framebuffer via SceVideoOut (built-in 8x8 bitmap font, no deps)
//   2. Native PS4 toast notifications via sceKernelSendNotificationRequest
//   3. Kernel debug log via printf / OrbisKernelDebugOutA (UART / klog)
// =============================================================================

#pragma once

#include "../session.hpp"

#include <string>

namespace bt {
namespace ui {

// =============================================================================
// Value formatting for display
// =============================================================================

/**
 * Format bytes into a human-readable unit (KB, MB, GB).
 * E.g. 1572864 → "1.5 MB"
 */
std::string format_bytes(int64_t bytes);

/**
 * Format speed in bytes/second.
 * E.g. 2621440.0 → "2.5 MB/s"
 */
std::string format_speed(double bps);

/**
 * Generate an ASCII progress bar of `width` characters.
 * E.g. pct=60.3, width=20 → "[############........]"
 */
std::string progress_bar(float pct, int width = 20);

// =============================================================================
// PS4 Native Notifications
// =============================================================================

/**
 * Send a native PS4 top-left toast notification on the TV screen.
 */
void notify(const std::string& message);

// =============================================================================
// MainScreen — main app screen (TV Framebuffer + Debug Console)
// =============================================================================

/**
 * Render an informational status screen on the TV and debug log.
 */
void render_status(const std::string& title, const std::string& detail);

/**
 * Render the current download state to the TV framebuffer and debug console.
 */
void render_progress(
    const std::string&  torrent_name,
    int64_t             total_size,
    const ProgressInfo& info
);

/**
 * Display an error message on screen and send a system notification.
 */
void render_error(const std::string& message);

/**
 * Display the completion screen with a download summary.
 */
void render_complete(const std::string& torrent_name, int64_t total_bytes, double elapsed_sec);

/**
 * Initialize PS4 SystemService, VideoOut framebuffer, UserService, and Pad.
 */
void init_system_services();

/**
 * Release VideoOut and Pad resources before exiting.
 */
void shutdown_system_services();

/**
 * Check whether the Circle (O) button was pressed (cancel).
 */
bool check_cancel_button();

/**
 * Check whether the Cross (X) button was pressed (confirm/exit).
 */
bool check_confirm_button();

} // namespace ui
} // namespace bt
