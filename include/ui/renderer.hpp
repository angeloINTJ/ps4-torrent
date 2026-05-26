// =============================================================================
// ui/renderer.hpp — UI layer for PS4 homebrew
//
// The PS4 homebrew has two rendering options:
//   A) Text via sceUserService + printf/OrbisKernelDebugOutA  → debug overlay
//   B) SDL2 (community-ported)                                → full graphical UI
//
// This implementation uses approach A as the "backend" and exposes
// a high-level screen-oriented API. The ui/sdl_renderer module
// (not included here) could implement the same interface with SDL2.
//
// The main screen displays:
//   ┌─────────────────────────────────────────────┐
//   │  PS4 Torrent  v0.1                          │
//   │                                             │
//   │  File: ubuntu-24.04.iso                     │
//   │  Size: 1.4 GB                               │
//   │                                             │
//   │  [████████████░░░░░░░░] 60.3%               │
//   │  Speed: 2.4 MB/s                            │
//   │  Peers: 12  Seeders: 45  Leechers: 78       │
//   │                                             │
//   │  ✓ Complete / ✗ Cancelled                   │
//   └─────────────────────────────────────────────┘
//
// On the PS4, output goes to the debug log (accessible via ps4debug/uart).
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
 * E.g. pct=60.3, width=20 → "[████████████░░░░░░░░]"
 */
std::string progress_bar(float pct, int width = 20);

// =============================================================================
// MainScreen — main app screen
// =============================================================================

/**
 * Render the current download state to the PS4 debug console.
 *
 * In the OpenOrbis environment, sceKernelDebugOutA (or printf via musl) writes
 * to UART/PS4debug. For a production UI, SDL2 or the SceVideoOut API
 * with OpenGL would be needed.
 *
 * @param torrent_name   Torrent/file name
 * @param total_size     Total size in bytes
 * @param info           ProgressInfo from the Session callback
 */
void render_progress(
    const std::string& torrent_name,
    int64_t            total_size,
    const ProgressInfo& info
);

/**
 * Display an error message on screen.
 */
void render_error(const std::string& message);

/**
 * Display the completion screen with a download summary.
 */
void render_complete(const std::string& torrent_name, int64_t total_bytes, double elapsed_sec);

/**
 * Initialize the PS4 UserService system (required for controller input).
 * No-op when called in a non-PS4 environment (for PC testing).
 */
void init_system_services();

/**
 * Check whether the Circle button was pressed (cancel).
 * Returns true once per press.
 */
bool check_cancel_button();

} // namespace ui
} // namespace bt
