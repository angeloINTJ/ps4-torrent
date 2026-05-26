#pragma once
// =============================================================================
// session.hpp — Orquestrador da sessão de download BitTorrent
//
// A Session conecta todos os módulos:
//   - Parseia o .torrent via Metainfo
//   - Anuncia ao tracker para obter peers
//   - Gerencia um pool de threads, uma por peer
//   - Delega download de blocos ao PieceManager
//   - Reporta progresso via callback
//
// Uso típico (no main):
//   bt::Session session("/mnt/usb/arquivo.torrent", "/data/downloads");
//   session.set_progress_callback([](float pct, ...) { /* atualiza UI */ });
//   session.start();         // Bloqueante até completar ou falhar
// =============================================================================

#include "metainfo.hpp"
#include "piece_manager.hpp"
#include "tracker.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bt {

// =============================================================================
// Informações de progresso passadas ao callback
// =============================================================================

struct ProgressInfo {
    float   pct;            // 0.0 – 100.0
    int64_t bytes_done;     // Bytes confirmados (peças completas)
    int64_t bytes_total;
    int     peers_active;   // Peers conectados no momento
    int     seeders;        // Último announce
    int     leechers;
    double  speed_bps;      // Taxa estimada em bytes/segundo
};

using ProgressCallback = std::function<void(const ProgressInfo&)>;

// =============================================================================
// Session
// =============================================================================

class Session {
public:
    // Número máximo de peers simultâneos
    static constexpr int MAX_PEERS = 30;

    // Porta local (0 = sem modo seeding, só download)
    static constexpr uint16_t LOCAL_PORT = 0;

    /**
     * @param torrent_path   Caminho para o arquivo .torrent
     * @param save_dir       Diretório de destino para os arquivos baixados
     */
    Session(const std::string& torrent_path, const std::string& save_dir);

    ~Session() { stop(); }

    // Não-copiável, não-movível (contém threads e atomics)
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // -------------------------------------------------------------------------
    // Controle
    // -------------------------------------------------------------------------

    /**
     * Registra um callback chamado periodicamente com o progresso.
     * Deve ser chamado antes de start().
     */
    void set_progress_callback(ProgressCallback cb);

    /**
     * Inicia o download.
     * Bloqueia a thread chamadora até o download completar, ocorrer erro fatal
     * ou stop() ser chamado de outra thread.
     *
     * @throws std::runtime_error  em falha irrecuperável
     */
    void start();

    /**
     * Sinaliza para parar. Thread-safe.
     * A sessão faz um announce "stopped" ao tracker antes de encerrar.
     */
    void stop() noexcept;

    bool is_complete() const noexcept;
    bool is_running()  const noexcept { return running_.load(); }

    // -------------------------------------------------------------------------
    // Informações sobre o torrent carregado
    // -------------------------------------------------------------------------
    const Metainfo& metainfo() const noexcept { return meta_; }

private:
    Metainfo                        meta_;
    std::string                     save_dir_;
    std::string                     peer_id_;    // Nosso ID de 20 chars gerado na inicialização

    std::unique_ptr<PieceManager>   pm_;
    ProgressCallback                progress_cb_;

    std::atomic<bool>               running_{false};
    std::atomic<bool>               stop_requested_{false};

    // Estado do último announce
    std::mutex          tracker_mutex_;
    TrackerResponse     last_tracker_resp_;

    // Contagem de peers ativos
    std::atomic<int>    peers_active_{0};

    // Estimativa de velocidade
    std::atomic<int64_t> bytes_at_last_check_{0};

    // -------------------------------------------------------------------------
    // Etapas internas
    // -------------------------------------------------------------------------

    /**
     * Gera um peer_id único no formato "-BT0001-XXXXXXXXXXXX"
     * (prefixo de cliente + 12 bytes aleatórios).
     */
    static std::string generate_peer_id();

    /**
     * Lê o arquivo .torrent do caminho fornecido.
     */
    static std::string read_file(const std::string& path);

    /**
     * Loop principal de um peer worker.
     * Executado em thread separada para cada peer.
     */
    void peer_worker(PeerAddr addr);

    /**
     * Thread de announce periódico ao tracker.
     */
    void tracker_thread();

    /**
     * Thread de progresso: chama progress_cb_ a cada ~500ms.
     */
    void progress_thread();

    /**
     * Lança até MAX_PEERS threads de peer_worker.
     * Substitui peers encerrados por novos quando disponíveis.
     */
    void run_peer_pool(const std::vector<PeerAddr>& peers);
};

} // namespace bt
