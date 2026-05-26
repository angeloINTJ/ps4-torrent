// =============================================================================
// session.cpp — Implementação da sessão BitTorrent
// =============================================================================

#include "session.hpp"
#include "peer_wire.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <thread>

namespace bt {

// =============================================================================
// Construtor — carrega o .torrent e inicializa os módulos
// =============================================================================

Session::Session(const std::string& torrent_path, const std::string& save_dir)
    : save_dir_(save_dir)
{
    std::string raw = read_file(torrent_path);
    meta_    = Metainfo::parse(raw);
    peer_id_ = generate_peer_id();
    pm_      = std::make_unique<PieceManager>(meta_, save_dir_);
}

// =============================================================================
// Helpers
// =============================================================================

std::string Session::read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("session: não foi possível abrir: " + path);

    // Obtém tamanho
    off_t size = ::lseek(fd, 0, SEEK_END);
    ::lseek(fd, 0, SEEK_SET);

    if (size <= 0) {
        ::close(fd);
        throw std::runtime_error("session: arquivo vazio ou inválido: " + path);
    }

    std::string data(static_cast<size_t>(size), '\0');
    ssize_t got = ::read(fd, data.data(), static_cast<size_t>(size));
    ::close(fd);

    if (got != size)
        throw std::runtime_error("session: leitura incompleta de: " + path);

    return data;
}

std::string Session::generate_peer_id() {
    // Formato: "-PS0001-" + 12 dígitos pseudo-aleatórios
    // PS = PS4 Torrent, 0001 = versão
    std::string id = "-PS0001-";
    id.reserve(20);

    // Semente baseada em tempo
    uint32_t seed = static_cast<uint32_t>(std::time(nullptr));
    for (int i = 0; i < 12; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        id += static_cast<char>('0' + (seed % 10));
    }

    return id;
}

void Session::set_progress_callback(ProgressCallback cb) {
    progress_cb_ = std::move(cb);
}

bool Session::is_complete() const noexcept {
    return pm_ && pm_->is_complete();
}

// =============================================================================
// stop
// =============================================================================

void Session::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

// =============================================================================
// peer_worker — loop de download de um único peer
// =============================================================================

void Session::peer_worker(PeerAddr addr) {
    peers_active_.fetch_add(1, std::memory_order_relaxed);

    // Escopo de limpeza automática ao sair (RAII-like sem exceptions aqui)
    struct Guard {
        std::atomic<int>& counter;
        ~Guard() { counter.fetch_sub(1, std::memory_order_relaxed); }
    } guard{peers_active_};

    try {
        PeerConnection conn;
        conn.connect(addr.ip, addr.port);

        if (!conn.handshake(meta_.info_hash, peer_id_)) return;

        // Envia nosso bitfield ao peer
        conn.send_bitfield(pm_->our_bitfield());

        // Informamos que estamos interessados
        conn.send_interested();

        // Bitfield do peer — acumulado à medida que chegam mensagens Have/Bitfield
        std::vector<uint8_t> peer_bf;

        // Número de requests em voo (pipeline simples)
        static constexpr int PIPELINE_DEPTH = 5;
        int requests_in_flight = 0;

        auto maybe_request = [&]() {
            while (requests_in_flight < PIPELINE_DEPTH && !conn.state().peer_choking) {
                auto req = pm_->next_request(peer_bf);
                if (!req) break;
                conn.send_request(req->piece_index, req->begin, req->length);
                ++requests_in_flight;
            }
        };

        // Loop principal de mensagens
        while (!stop_requested_.load() && !pm_->is_complete()) {
            auto msg_opt = conn.read_message();
            if (!msg_opt) {
                // Keep-alive — responde de volta
                conn.send_keepalive();
                continue;
            }

            const PeerMessage& msg = *msg_opt;

            switch (msg.type) {

            case MsgType::Bitfield:
                peer_bf = msg.bitfield;
                maybe_request();
                break;

            case MsgType::Have: {
                // Expande o bitfield se necessário
                size_t byte_needed = (msg.piece_index / 8) + 1;
                if (peer_bf.size() < byte_needed)
                    peer_bf.resize(byte_needed, 0);
                peer_bf[msg.piece_index / 8] |=
                    static_cast<uint8_t>(0x80 >> (msg.piece_index % 8));
                maybe_request();
                break;
            }

            case MsgType::Unchoke:
                // Peer nos desbloqueou — podemos pedir blocos agora
                maybe_request();
                break;

            case MsgType::Choke:
                // Peer nos bloqueou — cancela requests em voo (não precisa
                // enviar Cancel pois o peer ignorará mesmo)
                requests_in_flight = 0;
                break;

            case MsgType::Piece:
                // Recebe um bloco de dados
                if (requests_in_flight > 0) --requests_in_flight;
                pm_->receive_block(msg.piece_index, msg.begin, msg.block);

                // Se a peça completou, anuncia Have a este peer
                // (num sistema multi-peer completo, anunciaria a todos)
                if (pm_->pieces_done() > 0) {
                    conn.send_have(msg.piece_index);
                }

                // Solicita mais blocos para manter o pipeline cheio
                maybe_request();
                break;

            default:
                // Outras mensagens ignoradas por ora
                break;
            }
        }

    } catch (const std::exception& /* e */) {
        // Erros de conexão são esperados — peer pode cair a qualquer momento
        // Em modo debug: logar e.what() para o console PS4
    }
}

// =============================================================================
// tracker_thread — anuncia periodicamente ao tracker
// =============================================================================

void Session::tracker_thread() {
    while (!stop_requested_.load() && !pm_->is_complete()) {
        try {
            std::string event = "started";
            {
                std::lock_guard<std::mutex> lk(tracker_mutex_);
                // Nas chamadas seguintes, sem event especial
                if (last_tracker_resp_.interval > 0) event = "";
            }

            TrackerResponse resp = tracker_announce(
                meta_, peer_id_, LOCAL_PORT,
                /*uploaded=*/   0,
                /*downloaded*/  pm_->bytes_downloaded(),
                /*left=*/       pm_->bytes_total() - pm_->bytes_downloaded(),
                event
            );

            {
                std::lock_guard<std::mutex> lk(tracker_mutex_);
                last_tracker_resp_ = resp;
            }

            // Aguarda o intervalo pedido pelo tracker (ou 30s mínimo)
            int wait_sec = std::max(30, resp.interval);
            for (int i = 0; i < wait_sec && !stop_requested_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

        } catch (...) {
            // Falha de tracker não é fatal — espera e tenta de novo
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }

    // Announce final de "stopped" ou "completed"
    try {
        std::string final_event = pm_->is_complete() ? "completed" : "stopped";
        tracker_announce(
            meta_, peer_id_, LOCAL_PORT,
            0, pm_->bytes_downloaded(),
            pm_->bytes_total() - pm_->bytes_downloaded(),
            final_event
        );
    } catch (...) {}
}

// =============================================================================
// progress_thread — dispara callback a cada 500ms
// =============================================================================

void Session::progress_thread() {
    using Clock = std::chrono::steady_clock;
    auto last_time  = Clock::now();
    int64_t last_bytes = pm_->bytes_downloaded();

    while (!stop_requested_.load() && !pm_->is_complete()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (!progress_cb_) continue;

        auto    now        = Clock::now();
        int64_t cur_bytes  = pm_->bytes_downloaded();
        double  elapsed_s  = std::chrono::duration<double>(now - last_time).count();
        double  speed      = (elapsed_s > 0)
                               ? (cur_bytes - last_bytes) / elapsed_s
                               : 0.0;

        last_time  = now;
        last_bytes = cur_bytes;

        ProgressInfo info{};
        info.pct          = pm_->progress_pct();
        info.bytes_done   = cur_bytes;
        info.bytes_total  = pm_->bytes_total();
        info.peers_active = peers_active_.load();
        info.speed_bps    = speed;

        {
            std::lock_guard<std::mutex> lk(tracker_mutex_);
            info.seeders  = last_tracker_resp_.complete;
            info.leechers = last_tracker_resp_.incomplete;
        }

        progress_cb_(info);
    }

    // Callback final (100%)
    if (progress_cb_ && pm_->is_complete()) {
        ProgressInfo info{};
        info.pct        = 100.0f;
        info.bytes_done = pm_->bytes_total();
        info.bytes_total = pm_->bytes_total();
        progress_cb_(info);
    }
}

// =============================================================================
// run_peer_pool — gerencia o pool de peer workers
// =============================================================================

void Session::run_peer_pool(const std::vector<PeerAddr>& peers) {
    std::vector<std::thread> workers;
    workers.reserve(MAX_PEERS);

    size_t peer_idx = 0;

    while (!stop_requested_.load() && !pm_->is_complete()) {
        // Lança workers até o limite
        while (peers_active_.load() < MAX_PEERS && peer_idx < peers.size()) {
            PeerAddr addr = peers[peer_idx++];
            workers.emplace_back([this, addr]() { peer_worker(addr); });
        }

        // Se esgotamos os peers do tracker, aguarda novas respostas
        if (peer_idx >= peers.size() && peers_active_.load() == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Aguarda todos os workers terminarem
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}

// =============================================================================
// start — ponto de entrada público (bloqueante)
// =============================================================================

void Session::start() {
    running_.store(true);
    stop_requested_.store(false);

    // Announce inicial ao tracker
    TrackerResponse resp = tracker_announce(
        meta_, peer_id_, LOCAL_PORT,
        0, 0, meta_.total_length, "started"
    );

    if (!resp.ok())
        throw std::runtime_error("session: tracker recusou: " + resp.failure);

    {
        std::lock_guard<std::mutex> lk(tracker_mutex_);
        last_tracker_resp_ = resp;
    }

    // Inicia threads auxiliares
    std::thread t_tracker([this]() { tracker_thread();  });
    std::thread t_progress([this]() { progress_thread(); });

    // Roda o pool de peers (bloqueante)
    run_peer_pool(resp.peers);

    // Sinaliza parada para threads auxiliares
    stop_requested_.store(true);

    if (t_tracker.joinable())  t_tracker.join();
    if (t_progress.joinable()) t_progress.join();

    running_.store(false);
}

} // namespace bt
