#pragma once
// =============================================================================
// peer_wire.hpp — Protocolo wire BitTorrent (BEP 3)
//
// Gerencia uma conexão TCP com um peer, incluindo:
//   - Handshake inicial
//   - Envio e recebimento de mensagens tipadas
//   - Controle de choke/unchoke/interested
//   - Solicitação de blocos (request/piece)
//
// Cada instância de PeerConnection representa UMA conexão com UM peer.
// Para múltiplos peers, crie múltiplas instâncias (geralmente em threads).
// =============================================================================

#include "sha1.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// =============================================================================
// Tipos de mensagem do protocolo BitTorrent (BEP 3)
// =============================================================================

enum class MsgType : uint8_t {
    Choke         = 0,
    Unchoke       = 1,
    Interested    = 2,
    NotInterested = 3,
    Have          = 4,
    Bitfield      = 5,
    Request       = 6,
    Piece         = 7,
    Cancel        = 8,
    // Extensões (não implementadas ainda):
    // Port       = 9,   // DHT (BEP 5)
};

/**
 * Mensagem recebida de um peer, já desserializada.
 *
 * Campos válidos dependem do tipo:
 *   Have         → piece_index
 *   Bitfield     → bitfield
 *   Request      → piece_index, begin, length
 *   Piece        → piece_index, begin, block
 *   Cancel       → piece_index, begin, length
 *   Outros       → (sem payload)
 */
struct PeerMessage {
    MsgType  type;

    uint32_t piece_index = 0;
    uint32_t begin       = 0;
    uint32_t length      = 0;

    std::vector<uint8_t> bitfield;  // Para Bitfield
    std::vector<uint8_t> block;     // Para Piece (dados reais)
};

// =============================================================================
// Estado local de um peer (flags de choke/interest)
// =============================================================================

struct PeerState {
    bool am_choking      = true;   // Nós estamos chokando o peer
    bool am_interested   = false;  // Nós estamos interessados no peer
    bool peer_choking    = true;   // O peer está nos chokando
    bool peer_interested = false;  // O peer está interessado em nós
};

// =============================================================================
// PeerConnection
// =============================================================================

/**
 * Conexão com um único peer BitTorrent.
 *
 * Ciclo de vida típico:
 *   1. connect(ip, port)
 *   2. handshake(info_hash, peer_id)  → retorna false se info_hash não bater
 *   3. send_interested()
 *   4. Loop: read_message() → processa → send_request() → read_message() ...
 *   5. disconnect()
 */
class PeerConnection {
public:
    // Tamanho padrão de bloco: 16 KiB (padrão da spec BitTorrent)
    static constexpr uint32_t BLOCK_SIZE = 16 * 1024;

    PeerConnection() = default;
    ~PeerConnection() { disconnect(); }

    // Não-copiável (ownership do socket fd)
    PeerConnection(const PeerConnection&)            = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    // Movível
    PeerConnection(PeerConnection&& o) noexcept
        : fd_(o.fd_), state_(o.state_), peer_id_(std::move(o.peer_id_))
    {
        o.fd_ = -1;
    }

    // -------------------------------------------------------------------------
    // Gerenciamento de conexão
    // -------------------------------------------------------------------------

    /**
     * Conecta ao peer (TCP).
     * ip em network byte order (como retornado pelo tracker).
     * Lança std::runtime_error em falha.
     */
    void connect(uint32_t ip, uint16_t port);

    /**
     * Executa o handshake BitTorrent.
     * Envia nosso handshake e verifica o do peer.
     *
     * @return true  — handshake OK, info_hash confere
     * @return false — info_hash não confere (descarte a conexão)
     */
    bool handshake(const SHA1::Digest& info_hash, const std::string& our_peer_id);

    void disconnect() noexcept;

    bool is_connected() const noexcept { return fd_ >= 0; }

    // -------------------------------------------------------------------------
    // Leitura de mensagens
    // -------------------------------------------------------------------------

    /**
     * Lê a próxima mensagem do peer.
     *
     * @return std::nullopt  — keep-alive (mensagem de tamanho zero)
     * @return PeerMessage   — mensagem recebida e desserializada
     * @throws std::runtime_error em erro de protocolo ou socket
     */
    std::optional<PeerMessage> read_message();

    // -------------------------------------------------------------------------
    // Envio de mensagens (nomes autoexplicativos)
    // -------------------------------------------------------------------------

    void send_keepalive();
    void send_choke();
    void send_unchoke();
    void send_interested();
    void send_not_interested();
    void send_have(uint32_t piece_index);
    void send_bitfield(const std::vector<uint8_t>& bitfield);

    /**
     * Solicita um bloco de dados ao peer.
     *
     * @param piece_index  Índice da peça (0-based)
     * @param begin        Offset em bytes dentro da peça
     * @param length       Tamanho do bloco (tipicamente BLOCK_SIZE, exceto o último)
     */
    void send_request(uint32_t piece_index, uint32_t begin, uint32_t length);
    void send_cancel (uint32_t piece_index, uint32_t begin, uint32_t length);

    // -------------------------------------------------------------------------
    // Estado
    // -------------------------------------------------------------------------

    const PeerState& state()   const noexcept { return state_;   }
    const std::string& peer_id() const noexcept { return peer_id_; }

private:
    int         fd_      = -1;
    PeerState   state_;
    std::string peer_id_;  // 20-byte peer ID do peer remoto (obtido no handshake)

    // -------------------------------------------------------------------------
    // I/O de baixo nível
    // -------------------------------------------------------------------------

    // Envia exatamente `len` bytes (loop sobre send)
    void send_raw(const void* data, size_t len);

    // Recebe exatamente `len` bytes (loop sobre recv)
    void recv_raw(void*  data, size_t len);

    // Envia mensagem com prefixo de comprimento (4 bytes BE) + type byte + payload
    void send_msg(MsgType type, const uint8_t* payload = nullptr, size_t payload_len = 0);

    // Lê uint32_t big-endian do socket
    uint32_t recv_u32();

    // Escreve uint32_t big-endian no buffer
    static void write_u32(uint8_t* buf, uint32_t v) noexcept;
};

} // namespace bt
