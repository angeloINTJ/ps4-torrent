// =============================================================================
// main.cpp — PS4 Torrent homebrew entry point
//
// Flow:
//   1. Initialize system services (SCE UserService, Pad)
//   2. Locate the .torrent file on USB/HDD
//   3. Start the download Session with a progress callback
//   4. Main loop monitors controller input
//   5. Display completion or error screen
//
// Torrent location:
//   The app reads the first .torrent file found in /data/pkg/torrents/
//   (a mounted directory on consoles with ps4debug payload active).
//   A file picker may be implemented in the future.
//
// Download directory:
//   /data/pkg/downloads/  — requires ps4debug or GoldHEN for write access.
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
#include <string>

// =============================================================================
// Path configuration on the PS4
// =============================================================================

static constexpr const char* TORRENT_DIR  = "/data/pkg/torrents";
static constexpr const char* DOWNLOAD_DIR = "/data/pkg/downloads";

// =============================================================================
// find_first_torrent — find the first .torrent file in a directory
// =============================================================================

static std::string find_first_torrent(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        printf("[main] Could not open: %s\n", dir_path);
        return {};
    }

    std::string found;
    struct dirent* entry = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Check .torrent extension
        if (name.size() > 8 &&
            name.substr(name.size() - 8) == ".torrent")
        {
            found  = std::string(dir_path) + '/' + name;
            break;
        }
    }

    closedir(dir);
    return found;
}

// =============================================================================
// wait_for_cross_button — wait for the X button to confirm
// =============================================================================

static void wait_for_cross_button() {
    // Wait 5 seconds or until the X button is pressed
    // (in debug mode, any key works)
    for (int i = 0; i < 50; ++i) {
        sceKernelUsleep(100'000); // 100ms
        // bt::ui::check_cancel_button() uses Circle (O), here we just
        // use a timeout to simplify the final screen
    }
}

// =============================================================================
// eboot_main — replaces main() in the PS4 ABI
//
// In OpenOrbis, the entry point is standard main(), linked against musl libc.
// The name eboot_main is just a descriptive alias here.
// =============================================================================

int main() {
    using Clock = std::chrono::steady_clock;

    // -------------------------------------------------------------------------
    // 1. Initialize system services
    // -------------------------------------------------------------------------
    bt::ui::init_system_services();

    printf("[main] PS4 Torrent v0.1 starting...\n");
    printf("[main] Looking for .torrent in: %s\n", TORRENT_DIR);

    // -------------------------------------------------------------------------
    // 2. Locate the .torrent file
    // -------------------------------------------------------------------------
    std::string torrent_path = find_first_torrent(TORRENT_DIR);
    if (torrent_path.empty()) {
        bt::ui::render_error(
            std::string("No .torrent file found in:\n  ") + TORRENT_DIR +
            "\n\n  Place a .torrent file in this directory and relaunch the app."
        );
        sceKernelSleep(10);
        return 1;
    }

    printf("[main] Torrent found: %s\n", torrent_path.c_str());

    // -------------------------------------------------------------------------
    // 3. Create the download session
    // -------------------------------------------------------------------------
    std::unique_ptr<bt::Session> session;
    try {
        session = std::make_unique<bt::Session>(torrent_path, DOWNLOAD_DIR);
    } catch (const std::exception& e) {
        bt::ui::render_error(
            std::string("Error reading torrent:\n  ") + e.what()
        );
        sceKernelSleep(10);
        return 1;
    }

    const bt::Metainfo& meta = session->metainfo();
    printf("[main] Torrent: %s  (%lld bytes, %zu pieces)\n",
           meta.name.c_str(),
           static_cast<long long>(meta.total_length),
           meta.num_pieces());

    // -------------------------------------------------------------------------
    // 4. Configure the progress callback
    // -------------------------------------------------------------------------
    session->set_progress_callback([&meta](const bt::ProgressInfo& info) {
        bt::ui::render_progress(meta.name, meta.total_length, info);
    });

    // -------------------------------------------------------------------------
    // 5. Start the download in a separate thread to keep input responsive
    // -------------------------------------------------------------------------
    auto start_time   = Clock::now();
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

    // -------------------------------------------------------------------------
    // 6. Main loop — monitor controller input
    // -------------------------------------------------------------------------
    while (session->is_running() && !session->is_complete()) {
        sceKernelUsleep(100'000); // 100ms

        // Circle button = cancel download
        if (bt::ui::check_cancel_button()) {
            printf("[main] Cancel requested by user.\n");
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
        sceKernelSleep(10);
        return 1;
    }

    if (session->is_complete()) {
        bt::ui::render_complete(meta.name, meta.total_length, elapsed_s);
    } else {
        bt::ui::render_error("Download cancelled by user.");
    }

    wait_for_cross_button();
    return 0;
}
