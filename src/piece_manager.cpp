// =============================================================================
// piece_manager.cpp — Implementação do gerenciador de peças
// =============================================================================

#include "piece_manager.hpp"

#include <dirent.h>     // opendir, mkdir
#include <fcntl.h>      // open, O_CREAT, O_WRONLY
#include <sys/stat.h>   // stat, mkdir
#include <unistd.h>     // write, close, lseek

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bt {

// =============================================================================
// Constantes
// =============================================================================

static constexpr uint32_t BLOCK_SIZE = 16 * 1024; // 16 KiB

// =============================================================================
// Construtor
// =============================================================================

PieceManager::PieceManager(const Metainfo& meta, const std::string& save_dir)
    : meta_(meta)
    , save_dir_(save_dir)
{
    // Pré-aloca a estrutura de cada peça
    pieces_.reserve(meta.num_pieces());

    for (size_t i = 0; i < meta.num_pieces(); ++i) {
        int64_t  ps         = meta.piece_size(i);
        size_t   num_blocks = meta.num_blocks_in_piece(i);
        pieces_.emplace_back(static_cast<uint32_t>(i), ps, num_blocks);
    }

    // Cria diretórios e pré-aloca arquivos no sistema de arquivos
    allocate_files();
}

// =============================================================================
// receive_block
// =============================================================================

bool PieceManager::receive_block(
    uint32_t piece_index,
    uint32_t begin,
    const std::vector<uint8_t>& data)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (piece_index >= pieces_.size())
        return false;

    Piece& p = pieces_[piece_index];

    // Ignora blocos de peças já completas ou em falha de hash sendo reprocessadas
    if (p.status == PieceStatus::Complete) return false;

    // Valida offset e tamanho
    if (begin + data.size() > p.data.size())
        return false;

    // Determina o índice do bloco
    uint32_t block_idx = begin / BLOCK_SIZE;
    if (block_idx >= p.blocks_received.size())
        return false;

    // Copia o bloco para o buffer da peça
    std::memcpy(p.data.data() + begin, data.data(), data.size());

    // Marca o bloco como recebido (só conta uma vez)
    if (!p.blocks_received[block_idx]) {
        p.blocks_received[block_idx] = true;
        ++p.blocks_done;
        p.status = PieceStatus::Pending;
    }

    // Verifica se todos os blocos chegaram
    if (p.blocks_done == p.blocks_received.size()) {
        return verify_and_flush(p);
    }

    return false;
}

// =============================================================================
// verify_and_flush
// =============================================================================

bool PieceManager::verify_and_flush(Piece& p) {
    // Calcula SHA1 dos dados acumulados
    SHA1::Digest actual = SHA1::hash(p.data.data(), p.data.size());

    if (!SHA1::equal(actual, meta_.piece_hashes[p.index])) {
        // Hash falhou — reseta a peça para redownload
        p.status       = PieceStatus::HashFail;
        p.blocks_done  = 0;
        std::fill(p.blocks_received.begin(), p.blocks_received.end(), false);
        std::fill(p.data.begin(), p.data.end(), 0);
        return false;
    }

    // Grava no disco
    write_piece(p.index, p.data);

    // Libera a memória do buffer (não precisamos mais dos dados)
    p.data.clear();
    p.data.shrink_to_fit();
    p.status = PieceStatus::Complete;

    done_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// =============================================================================
// write_piece — grava uma peça nos arquivos corretos (single ou multi-file)
// =============================================================================

void PieceManager::write_piece(uint32_t piece_index, const std::vector<uint8_t>& data) {
    // Offset global desta peça no stream de bytes concatenados
    int64_t piece_global_begin = static_cast<int64_t>(piece_index) * meta_.piece_length;
    int64_t piece_global_end   = piece_global_begin + static_cast<int64_t>(data.size());

    // Itera pelos arquivos que se sobrepõem com o range desta peça
    for (const FileEntry& file : meta_.files) {
        int64_t file_begin = file.offset;
        int64_t file_end   = file.offset + file.length;

        // Sem sobreposição com esta peça?
        if (file_end <= piece_global_begin) continue;
        if (file_begin >= piece_global_end) break;  // Arquivos estão ordenados por offset

        // Intervalo de sobreposição em coordenadas globais
        int64_t overlap_begin = std::max(piece_global_begin, file_begin);
        int64_t overlap_end   = std::min(piece_global_end,   file_end);

        // Offset dentro do buffer da peça
        int64_t piece_offset = overlap_begin - piece_global_begin;
        // Offset dentro do arquivo
        int64_t file_offset  = overlap_begin - file_begin;
        // Número de bytes a gravar
        int64_t write_len    = overlap_end - overlap_begin;

        // Monta o caminho completo do arquivo
        std::string path = save_dir_;
        if (meta_.is_single_file()) {
            path += '/' + meta_.name;
        } else {
            path += '/' + file.path_str();
        }

        // Abre o arquivo para escrita (deve já existir após allocate_files)
        int fd = ::open(path.c_str(), O_WRONLY);
        if (fd < 0)
            throw std::runtime_error("piece_manager: falha ao abrir para escrita: " + path);

        // Posiciona o cursor no offset correto
        if (::lseek(fd, static_cast<off_t>(file_offset), SEEK_SET) < 0) {
            ::close(fd);
            throw std::runtime_error("piece_manager: lseek() falhou em: " + path);
        }

        // Grava com loop para lidar com writes parciais
        const uint8_t* src     = data.data() + piece_offset;
        size_t         rem     = static_cast<size_t>(write_len);
        while (rem > 0) {
            ssize_t written = ::write(fd, src, rem);
            if (written <= 0) {
                ::close(fd);
                throw std::runtime_error("piece_manager: write() falhou em: " + path);
            }
            src += written;
            rem -= static_cast<size_t>(written);
        }

        ::close(fd);
    }
}

// =============================================================================
// allocate_files — cria diretórios e arquivos esparsos
// =============================================================================

// Helper: cria um diretório e todos os pais necessários (mkdir -p)
static void mkdir_p(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            std::string sub = path.substr(0, i);
            ::mkdir(sub.c_str(), 0755); // Ignora erro se já existe
        }
    }
}

void PieceManager::allocate_files() {
    if (meta_.is_single_file()) {
        mkdir_p(save_dir_);

        std::string path = save_dir_ + '/' + meta_.name;
        // Cria o arquivo e define o tamanho via lseek + write de 1 byte
        int fd = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            throw std::runtime_error("piece_manager: não foi possível criar: " + path);

        // truncate: posiciona no último byte e escreve zero
        if (meta_.total_length > 0) {
            ::lseek(fd, static_cast<off_t>(meta_.total_length - 1), SEEK_SET);
            uint8_t zero = 0;
            [[maybe_unused]] ssize_t ignored = ::write(fd, &zero, 1);
        }
        ::close(fd);

    } else {
        // Multi-file: cria estrutura de diretórios e um arquivo por entrada
        for (const FileEntry& file : meta_.files) {
            // Reconstrói o caminho completo do arquivo
            std::string dir_path = save_dir_ + '/' + meta_.name;
            std::string file_path = dir_path;

            for (size_t i = 0; i < file.path.size(); ++i) {
                if (i + 1 < file.path.size()) {
                    dir_path += '/' + file.path[i];
                }
                file_path += '/' + file.path[i];
            }

            mkdir_p(dir_path);

            int fd = ::open(file_path.c_str(), O_CREAT | O_WRONLY, 0644);
            if (fd < 0)
                throw std::runtime_error(
                    "piece_manager: não foi possível criar: " + file_path);

            if (file.length > 0) {
                ::lseek(fd, static_cast<off_t>(file.length - 1), SEEK_SET);
                uint8_t zero = 0;
                [[maybe_unused]] ssize_t ignored2 = ::write(fd, &zero, 1);
            }
            ::close(fd);
        }
    }
}

// =============================================================================
// next_request — seleciona próximo bloco a solicitar
// =============================================================================

std::optional<BlockRequest> PieceManager::next_request(
    const std::vector<uint8_t>& peer_bitfield) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (const Piece& p : pieces_) {
        // Pula peças completas
        if (p.status == PieceStatus::Complete) continue;

        // O peer tem esta peça?
        if (!peer_has_piece(peer_bitfield, p.index)) continue;

        // Procura o primeiro bloco não recebido
        for (size_t bi = 0; bi < p.blocks_received.size(); ++bi) {
            if (p.blocks_received[bi]) continue;

            uint32_t begin = static_cast<uint32_t>(bi) * BLOCK_SIZE;

            // Tamanho do bloco: pode ser menor no último bloco da última peça
            int64_t  piece_sz    = meta_.piece_size(p.index);
            int64_t  remaining   = piece_sz - static_cast<int64_t>(begin);
            uint32_t block_len   = static_cast<uint32_t>(
                std::min(remaining, static_cast<int64_t>(BLOCK_SIZE)));

            return BlockRequest{p.index, begin, block_len};
        }
    }

    return std::nullopt; // Nada a solicitar
}

// =============================================================================
// our_bitfield
// =============================================================================

std::vector<uint8_t> PieceManager::our_bitfield() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // 1 bit por peça, arredondado para cima para bytes
    size_t num_bytes = (pieces_.size() + 7) / 8;
    std::vector<uint8_t> bf(num_bytes, 0);

    for (size_t i = 0; i < pieces_.size(); ++i) {
        if (pieces_[i].status == PieceStatus::Complete) {
            // Bit na posição i: byte = i/8, bit = 7 - (i%8) (MSB first)
            bf[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
        }
    }

    return bf;
}

// =============================================================================
// Progresso
// =============================================================================

int64_t PieceManager::bytes_downloaded() const noexcept {
    return static_cast<int64_t>(done_count_.load()) * meta_.piece_length;
}

float PieceManager::progress_pct() const noexcept {
    if (meta_.total_length == 0) return 100.0f;
    float ratio = static_cast<float>(bytes_downloaded()) /
                  static_cast<float>(meta_.total_length);
    return ratio * 100.0f;
}

// =============================================================================
// peer_has_piece
// =============================================================================

bool PieceManager::peer_has_piece(
    const std::vector<uint8_t>& bitfield, uint32_t index) noexcept
{
    size_t byte_idx = index / 8;
    if (byte_idx >= bitfield.size()) return false;
    return (bitfield[byte_idx] & (0x80 >> (index % 8))) != 0;
}

} // namespace bt
