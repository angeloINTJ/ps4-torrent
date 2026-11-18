// =============================================================================
// main.cpp — PS4 Torrent homebrew entry point (FW <= 11.02+)
//
// Flow:
//   1. Initialize system services (VideoOut framebuffer, SystemService, UserService, Pad)
//   2. Ensure /data/pkg/torrents and /data/pkg/downloads directories exist
//   3. Locate the first .torrent file in /data/pkg/torrents or USB (/mnt/usb0, /mnt/usb1)
//   4. Start the download Session with a progress callback
//   5. Main loop monitors controller input
//   6. Display completion or error screen on TV + native PS4 notification
// =============================================================================

// PS4 headers via OpenOrbis
#include <orbis/libkernel.h>    // sceKernelUsleep, sceKernelSleep
#include <orbis/UserService.h>

// Our library
#include "session.hpp"
#include "ui/renderer.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <string>
#include <vector>

// =============================================================================
// Path configuration on the PS4
// =============================================================================

static constexpr const char* TORRENT_SEARCH_DIRS[] = {
    "/data/pkg/torrents",
    "/mnt/usb0/torrents",
    "/mnt/usb0",
    "/mnt/usb1/torrents",
    "/mnt/usb1",
    "/data/torrents",
};

static constexpr const char* DEFAULT_DOWNLOAD_DIR = "/data/pkg/downloads";

// =============================================================================
// ensure_directories — create default directories on the PS4 HDD
// =============================================================================

static void ensure_directories() {
    ::mkdir("/data", 0777);
    ::mkdir("/data/pkg", 0777);
    ::mkdir("/data/pkg/torrents", 0777);
    ::mkdir("/data/pkg/downloads", 0777);
    ::mkdir("/data/torrents", 0777);
}

// =============================================================================
// find_first_torrent — search candidate directories for a .torrent file
// =============================================================================

static std::string find_first_torrent() {
    for (const char* dir_path : TORRENT_SEARCH_DIRS) {
        DIR* dir = opendir(dir_path);
        if (!dir) continue;

        std::string found;
        struct dirent* entry = nullptr;

        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 8 &&
                name.substr(name.size() - 8) == ".torrent")
            {
                found = std::string(dir_path) + '/' + name;
                break;
            }
        }

        closedir(dir);
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

// =============================================================================
// wait_for_exit_button — wait up to 30s or until [X] / [O] is pressed
// =============================================================================

static void wait_for_exit_button() {
    // Small debounce delay so a held button doesn't immediately trigger exit
    sceKernelUsleep(300'000);

    for (int i = 0; i < 300; ++i) {
        sceKernelUsleep(100'000); // 100ms
        if (bt::ui::check_confirm_button() || bt::ui::check_cancel_button()) {
            break;
        }
    }
}

// =============================================================================
// main — PS4 eboot entry point
// =============================================================================

int main() {
    using Clock = std::chrono::steady_clock;

    // -------------------------------------------------------------------------
    // 1. Initialize system services (VideoOut, SplashScreen, Pad, UserService)
    // -------------------------------------------------------------------------
    bt::ui::init_system_services();
    ensure_directories();

    printf("[main] PS4 Torrent v0.2 starting...\n");

    // -------------------------------------------------------------------------
    // 2. Locate the .torrent file
    // -------------------------------------------------------------------------
    std::string torrent_path = find_first_torrent();
    if (torrent_path.empty()) {
        bt::ui::render_error(
            "No .torrent file found!\n\n"
            "Place a .torrent file in one of these paths:\n"
            "  - /data/pkg/torrents/   (via FTP)\n"
            "  - /mnt/usb0/            (USB drive root)\n"
            "  - /mnt/usb0/torrents/   (USB drive folder)\n\n"
            "Note: /data/pkg/torrents has been created automatically."
        );
        wait_for_exit_button();
        bt::ui::shutdown_system_services();
        return 1;
    }

    printf("[main] Torrent found: %s\n", torrent_path.c_str());

    // -------------------------------------------------------------------------
    // 3. Create the download session
    // -------------------------------------------------------------------------
    bt::ui::render_status("Loading torrent...", torrent_path);

    std::unique_ptr<bt::Session> session;
    try {
        session = std::make_unique<bt::Session>(torrent_path, DEFAULT_DOWNLOAD_DIR);
    } catch (const std::exception& e) {
        bt::ui::render_error(
            std::string("Error reading torrent:\n  ") + e.what()
        );
        wait_for_exit_button();
        bt::ui::shutdown_system_services();
        return 1;
    }

    const bt::Metainfo& meta = session->metainfo();
    printf("[main] Torrent: %s  (%lld bytes, %zu pieces)\n",
           meta.name.c_str(),
           static_cast<long long>(meta.total_length),
           meta.num_pieces());

    bt::ui::notify("Starting: " + meta.name);
    bt::ui::render_status(
        "Connecting to tracker...",
        "Torrent: " + meta.name + " (" + bt::ui::format_bytes(meta.total_length) + ")"
    );

    // -------------------------------------------------------------------------
    // 4. Configure the progress callback
    // -------------------------------------------------------------------------
    session->set_progress_callback([&meta](const bt::ProgressInfo& info) {
        bt::ui::render_progress(meta.name, meta.total_length, info);
    });

    // -------------------------------------------------------------------------
    // 5. Start the download in a separate thread to keep input responsive
    // -------------------------------------------------------------------------
    auto start_time    = Clock::now();
    bool session_error = false;
    std::string error_msg;

    std::thread download_thread([&]() {
        try {
            session->start();
        } catch (const std::exception& e) {
            session_error = true;
            error_msg     = e.what();
        }
    });

    // Wait briefly for download_thread to set running_ or fail
    for (int i = 0; i < 20 && !session->is_running() && !session_error; ++i) {
        sceKernelUsleep(50'000);
    }

    // -------------------------------------------------------------------------
    // 6. Main loop — monitor controller input
    // -------------------------------------------------------------------------
    while (session->is_running() && !session->is_complete() && !session_error) {
        sceKernelUsleep(100'000); // 100ms

        // Circle button = cancel download
        if (bt::ui::check_cancel_button()) {
            printf("[main] Cancel requested by user.\n");
            bt::ui::render_status("Cancelling download...", "Stopping peer connections and notifying tracker...");
            session->stop();
            break;
        }
    }

    // Wait for the download thread to finish cleanly
    if (download_thread.joinable()) download_thread.join();

    auto end_time    = Clock::now();
    double elapsed_s = std::chrono::duration<double>(end_time - start_time).count();

    // -------------------------------------------------------------------------
    // 7. Final screen
    // -------------------------------------------------------------------------
    if (session_error) {
        bt::ui::render_error(std::string("Download error:\n  ") + error_msg);
        wait_for_exit_button();
        bt::ui::shutdown_system_services();
        return 1;
    }

    if (session->is_complete()) {
        bt::ui::render_complete(meta.name, meta.total_length, elapsed_s);
    } else {
        bt::ui::render_error("Download cancelled by user.");
    }

    wait_for_exit_button();
    bt::ui::shutdown_system_services();
    return 0;
}
