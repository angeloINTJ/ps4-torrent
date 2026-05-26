// =============================================================================
// main.cpp — Ponto de entrada do homebrew PS4 Torrent
//
// Fluxo:
//   1. Inicializa serviços do sistema (SCE UserService, Pad)
//   2. Localiza o arquivo .torrent no USB/HDD
//   3. Inicia a Session de download com callback de progresso
//   4. Mantém o loop principal monitorando o controle
//   5. Exibe tela de conclusão ou erro
//
// Localização do .torrent:
//   O app lê o primeiro arquivo .torrent encontrado em /data/pkg/torrents/
//   (diretório montado em consoles com payload ps4debug ativo).
//   Futuramente um seletor de arquivos pode ser implementado.
//
// Diretório de download:
//   /data/pkg/downloads/  — requer ps4debug ou goldhen para acesso de escrita.
// =============================================================================

// Cabeçalhos PS4 via OpenOrbis
#include <orbis/libkernel.h>    // sceKernelUsleep, sceKernelSleep
#include <orbis/UserService.h>

// Nossa biblioteca
#include "session.hpp"
#include "ui/renderer.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>

// =============================================================================
// Configurações de caminhos no PS4
// =============================================================================

static constexpr const char* TORRENT_DIR  = "/data/pkg/torrents";
static constexpr const char* DOWNLOAD_DIR = "/data/pkg/downloads";

// =============================================================================
// find_first_torrent — busca o primeiro .torrent no diretório
// =============================================================================

static std::string find_first_torrent(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        printf("[main] Não foi possível abrir: %s\n", dir_path);
        return {};
    }

    std::string found;
    struct dirent* entry = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Verifica extensão .torrent
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
// wait_for_cross_button — aguarda o botão X para confirmar
// =============================================================================

static void wait_for_cross_button() {
    // Aguarda 5 segundos ou até o botão X ser pressionado
    // (em modo debug, qualquer tecla funciona)
    for (int i = 0; i < 50; ++i) {
        sceKernelUsleep(100'000); // 100ms
        // bt::ui::check_cancel_button() usa O (circle), aqui aguardamos
        // apenas timeout para simplificar a tela final
    }
}

// =============================================================================
// eboot_main — substitui main() no ABI do PS4
//
// No OpenOrbis, o ponto de entrada é main() padrão, ligado ao musl libc.
// O nome eboot_main é apenas um alias descritivo aqui.
// =============================================================================

int main() {
    using Clock = std::chrono::steady_clock;

    // -------------------------------------------------------------------------
    // 1. Inicializa serviços do sistema
    // -------------------------------------------------------------------------
    bt::ui::init_system_services();

    printf("[main] PS4 Torrent v0.1 iniciando...\n");
    printf("[main] Buscando .torrent em: %s\n", TORRENT_DIR);

    // -------------------------------------------------------------------------
    // 2. Localiza o arquivo .torrent
    // -------------------------------------------------------------------------
    std::string torrent_path = find_first_torrent(TORRENT_DIR);
    if (torrent_path.empty()) {
        bt::ui::render_error(
            std::string("Nenhum arquivo .torrent encontrado em:\n  ") + TORRENT_DIR +
            "\n\n  Coloque um arquivo .torrent neste diretório e relance o app."
        );
        sceKernelSleep(10);
        return 1;
    }

    printf("[main] Torrent encontrado: %s\n", torrent_path.c_str());

    // -------------------------------------------------------------------------
    // 3. Cria a sessão de download
    // -------------------------------------------------------------------------
    std::unique_ptr<bt::Session> session;
    try {
        session = std::make_unique<bt::Session>(torrent_path, DOWNLOAD_DIR);
    } catch (const std::exception& e) {
        bt::ui::render_error(
            std::string("Erro ao ler o torrent:\n  ") + e.what()
        );
        sceKernelSleep(10);
        return 1;
    }

    const bt::Metainfo& meta = session->metainfo();
    printf("[main] Torrent: %s  (%lld bytes, %zu pecas)\n",
           meta.name.c_str(),
           static_cast<long long>(meta.total_length),
           meta.num_pieces());

    // -------------------------------------------------------------------------
    // 4. Configura o callback de progresso
    // -------------------------------------------------------------------------
    session->set_progress_callback([&meta](const bt::ProgressInfo& info) {
        bt::ui::render_progress(meta.name, meta.total_length, info);
    });

    // -------------------------------------------------------------------------
    // 5. Inicia o download em thread separada para não bloquear o input
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
    // 6. Loop principal — verifica input do controle
    // -------------------------------------------------------------------------
    while (session->is_running() && !session->is_complete()) {
        sceKernelUsleep(100'000); // 100ms

        // Botão O = cancelar download
        if (bt::ui::check_cancel_button()) {
            printf("[main] Cancelamento solicitado pelo usuario.\n");
            session->stop();
            break;
        }
    }

    // Aguarda a thread de download encerrar limpo
    if (download_thread.joinable()) download_thread.join();

    auto end_time    = Clock::now();
    double elapsed_s = std::chrono::duration<double>(end_time - start_time).count();

    // -------------------------------------------------------------------------
    // 7. Tela final
    // -------------------------------------------------------------------------
    if (session_error) {
        bt::ui::render_error(std::string("Erro durante o download:\n  ") + error_msg);
        sceKernelSleep(10);
        return 1;
    }

    if (session->is_complete()) {
        bt::ui::render_complete(meta.name, meta.total_length, elapsed_s);
    } else {
        bt::ui::render_error("Download cancelado pelo usuario.");
    }

    wait_for_cross_button();
    return 0;
}
