#pragma once
// =============================================================================
// metainfo.hpp — Parser de arquivos .torrent (BEP 3 + BEP 12)
//
// Suporta:
//   - Torrents single-file e multi-file
//   - announce e announce-list (multi-tracker)
//   - Cálculo automático do info_hash (SHA1 do info dict raw)
// =============================================================================

#include "bencode.hpp"
#include "sha1.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bt {

// =============================================================================
// Estruturas de dados
// =============================================================================

/**
 * Uma entrada de arquivo dentro de um torrent multi-file.
 *
 * Exemplo de torrent com estrutura:
 *   MeuAlbum/
 *     faixa01.flac   → path={"MeuAlbum","faixa01.flac"}, length=...
 *     faixa02.flac   → path={"MeuAlbum","faixa02.flac"}, length=...
 */
struct FileEntry {
    std::vector<std::string> path;    // Componentes do caminho (sem separador)
    int64_t                  length;  // Tamanho em bytes

    // Offset acumulado em relação ao início do torrent (preenchido pelo parser)
    int64_t offset = 0;

    // Retorna o caminho como string com '/' como separador
    std::string path_str() const;
};

/**
 * Metainfo completo de um arquivo .torrent.
 *
 * O info_hash é calculado internamente durante o parse() e é o identificador
 * único do torrent na rede BitTorrent.
 */
struct Metainfo {
    // -------------------------------------------------------------------------
    // Campos do torrent
    // -------------------------------------------------------------------------
    std::string announce;                             // URL do tracker principal
    std::vector<std::vector<std::string>> announce_list; // BEP 12: lista de trackers

    std::string name;          // Nome sugerido do arquivo ou diretório
    int64_t     piece_length;  // Tamanho de cada peça em bytes (ex: 262144 = 256 KiB)
    int64_t     total_length;  // Soma do tamanho de todos os arquivos

    // Hashes SHA1 de cada peça (20 bytes cada)
    // piece_hashes[i] = SHA1 dos bytes da peça i
    std::vector<SHA1::Digest> piece_hashes;

    // Lista de arquivos. Em torrents single-file, tem exatamente um elemento
    // com path vazio (usa-se o campo `name` como nome do arquivo).
    std::vector<FileEntry> files;

    // SHA1 do info dict bencoded bruto — identificador do torrent na rede
    SHA1::Digest info_hash;

    // -------------------------------------------------------------------------
    // Consultas úteis
    // -------------------------------------------------------------------------

    bool is_single_file() const noexcept {
        return files.size() == 1 && files[0].path.empty();
    }

    size_t num_pieces() const noexcept {
        return piece_hashes.size();
    }

    /**
     * Calcula o tamanho real da peça `index`.
     * A última peça pode ser menor que piece_length.
     */
    int64_t piece_size(size_t index) const noexcept {
        if (index + 1 < num_pieces()) return piece_length;
        return total_length - static_cast<int64_t>(index) * piece_length;
    }

    /**
     * Número de blocos de 16 KiB na peça `index`.
     * A última peça (e o último bloco dela) podem ser menores.
     */
    size_t num_blocks_in_piece(size_t index) const noexcept {
        static constexpr int64_t BLOCK_SIZE = 16 * 1024;
        int64_t ps = piece_size(index);
        return static_cast<size_t>((ps + BLOCK_SIZE - 1) / BLOCK_SIZE);
    }

    // -------------------------------------------------------------------------
    // Fábrica
    // -------------------------------------------------------------------------

    /**
     * Parseia um arquivo .torrent lido como string de bytes.
     *
     * @param raw   Conteúdo completo do .torrent (bytes binários)
     * @throws std::runtime_error   se o arquivo estiver malformado
     */
    static Metainfo parse(const std::string& raw);
};

} // namespace bt
