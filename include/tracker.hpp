#pragma once
// =============================================================================
// tracker.hpp — Cliente HTTP para tracker BitTorrent (BEP 3)
//
// Implementado diretamente sobre BSD sockets (sem libcurl) para minimizar
// dependências no ambiente OpenOrbis/PS4.
//
// Limitações desta versão:
//   - Apenas HTTP (não HTTPS) — O PS4 tem SceSsl mas adiciona complexidade
//   - Apenas formato compacto de peers (compact=1), padrão na prática
//   - UDP trackers (BEP 15) não implementados ainda
// =============================================================================

#include "metainfo.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace bt {

/**
 * Endereço IPv4 de um peer retornado pelo tracker.
 */
struct PeerAddr {
    uint32_t ip;    // Endereço em network byte order (big-endian)
    uint16_t port;  // Porta em host byte order

    // Retorna IP como string "x.x.x.x"
    std::string ip_str() const;
};

/**
 * Resposta parseada do tracker.
 */
struct TrackerResponse {
    int32_t               interval   = 1800; // Intervalo em segundos até próximo announce
    int32_t               min_interval = 0;  // Intervalo mínimo (opcional)
    int32_t               complete   = 0;    // Seeders
    int32_t               incomplete = 0;    // Leechers
    std::vector<PeerAddr> peers;             // Lista de peers

    std::string failure;  // Não-vazio em caso de erro do tracker

    bool ok() const noexcept { return failure.empty(); }
};

// =============================================================================
// API pública
// =============================================================================

/**
 * Realiza um HTTP GET no tracker e retorna a resposta parseada.
 *
 * @param metainfo    Metainfo do torrent (para info_hash e announce URL)
 * @param peer_id     Identificador de 20 bytes do nosso cliente (gerado na sessão)
 * @param port        Porta que estamos ouvindo (0 = apenas leeching)
 * @param uploaded    Bytes enviados até agora
 * @param downloaded  Bytes recebidos até agora
 * @param left        Bytes restantes para download completo
 * @param event       "started" | "stopped" | "completed" | "" (regular)
 *
 * @throws std::runtime_error  em falha de rede ou resposta inválida
 */
TrackerResponse tracker_announce(
    const Metainfo&    metainfo,
    const std::string& peer_id,
    uint16_t           port,
    int64_t            uploaded,
    int64_t            downloaded,
    int64_t            left,
    const std::string& event = "started"
);

/**
 * Encoda bytes brutos para percent-encoding (RFC 3986).
 * Todos os bytes fora de [A-Za-z0-9\-_.~] são codificados como %XX.
 */
std::string url_encode(std::string_view raw);

} // namespace bt
