#pragma once
// =============================================================================
// piece_manager.hpp — Gerenciador de peças do torrent
//
// Responsabilidades:
//   - Rastrear quais peças foram baixadas e verificadas
//   - Verificar integridade de cada peça via SHA1
//   - Montar o bitfield para anunciar aos peers
//   - Selecionar a próxima peça/bloco a solicitar (estratégia rarest-first simples)
//   - Gravar blocos recebidos no arquivo de destino
//
// Thread-safety:
//   Todos os métodos públicos são protegidos por um mutex interno.
//   É seguro chamar de múltiplas threads de peer simultaneamente.
// =============================================================================

#include "metainfo.hpp"
#include "sha1.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// =============================================================================
// Estado de uma peça individual
// =============================================================================

enum class PieceStatus : uint8_t {
    Missing    = 0,   // Não temos e não estamos baixando
    Pending    = 1,   // Blocos sendo baixados
    Complete   = 2,   // Todos blocos recebidos e SHA1 verificado
    HashFail   = 3,   // Recebida mas SHA1 errado — será redownloadada
};

/**
 * Representa uma peça em processo de download.
 * Cada peça é dividida em blocos de 16 KiB (BLOCK_SIZE).
 */
struct Piece {
    uint32_t              index;
    PieceStatus           status;
    std::vector<uint8_t>  data;              // Buffer acumulando os blocos
    std::vector<bool>     blocks_received;   // Bitset: bloco i chegou?
    uint32_t              blocks_done;       // Contagem para evitar loop de verificação

    explicit Piece(uint32_t idx, int64_t piece_len, size_t num_blocks)
        : index(idx)
        , status(PieceStatus::Missing)
        , data(static_cast<size_t>(piece_len), 0)
        , blocks_received(num_blocks, false)
        , blocks_done(0)
    {}
};

// =============================================================================
// Solicitação de bloco — unidade mínima de download
// =============================================================================

struct BlockRequest {
    uint32_t piece_index;
    uint32_t begin;    // Offset em bytes dentro da peça
    uint32_t length;   // Tamanho do bloco (≤ 16 KiB)
};

// =============================================================================
// PieceManager
// =============================================================================

class PieceManager {
public:
    /**
     * @param meta      Metainfo do torrent
     * @param save_dir  Diretório onde os arquivos serão gravados
     *                  (ex: "/data/pkg" no PS4 jailbroken)
     */
    PieceManager(const Metainfo& meta, const std::string& save_dir);

    // -------------------------------------------------------------------------
    // Interface principal (thread-safe)
    // -------------------------------------------------------------------------

    /**
     * Recebe um bloco de dados de um peer.
     * Se a peça completar, verifica o SHA1 e grava no disco.
     *
     * @return true   — peça completa e verificada com sucesso
     * @return false  — bloco armazenado mas peça ainda incompleta, ou SHA1 falhou
     */
    bool receive_block(uint32_t piece_index, uint32_t begin,
                       const std::vector<uint8_t>& data);

    /**
     * Seleciona o próximo bloco a solicitar para um peer específico.
     * Usa estratégia simples: primeira peça Missing, primeiro bloco faltando.
     *
     * @param peer_bitfield   Bitfield do peer (quais peças ele tem)
     * @return BlockRequest   — bloco a solicitar
     * @return std::nullopt   — nada a solicitar (download completo ou peer não tem nada útil)
     */
    std::optional<BlockRequest> next_request(const std::vector<uint8_t>& peer_bitfield) const;

    /**
     * Retorna nosso bitfield atual (para enviar ao peer no handshake).
     * 1 bit por peça, ordem MSB-first.
     */
    std::vector<uint8_t> our_bitfield() const;

    // -------------------------------------------------------------------------
    // Progresso
    // -------------------------------------------------------------------------

    size_t  pieces_total()    const noexcept { return pieces_.size(); }
    size_t  pieces_done()     const noexcept { return done_count_.load(); }
    bool    is_complete()     const noexcept { return done_count_ == pieces_.size(); }

    int64_t bytes_downloaded() const noexcept;
    int64_t bytes_total()      const noexcept { return meta_.total_length; }

    /** Percentual de 0.0 a 100.0 */
    float   progress_pct()     const noexcept;

private:
    const Metainfo&           meta_;
    std::string               save_dir_;
    std::vector<Piece>        pieces_;
    std::atomic<size_t>       done_count_{0};

    mutable std::mutex        mutex_;   // Protege pieces_

    // -------------------------------------------------------------------------
    // Helpers internos (chamados com mutex_ travado)
    // -------------------------------------------------------------------------

    /**
     * Verifica SHA1 de uma peça completa e, se OK, grava no disco.
     * Se SHA1 falhar, marca a peça como HashFail para redownload.
     */
    bool verify_and_flush(Piece& p);

    /**
     * Escreve os bytes de uma peça nos arquivos corretos.
     * Lida com peças que cruzam fronteiras de arquivo (modo multi-file).
     */
    void write_piece(uint32_t piece_index, const std::vector<uint8_t>& data);

    /**
     * Cria os diretórios e arquivos necessários com tamanho pré-alocado.
     * Chamado no construtor.
     */
    void allocate_files();

    /**
     * Verifica se o peer tem a peça `index` baseado no bitfield dele.
     */
    static bool peer_has_piece(const std::vector<uint8_t>& bitfield, uint32_t index) noexcept;
};

} // namespace bt
