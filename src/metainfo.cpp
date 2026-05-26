// =============================================================================
// metainfo.cpp — Implementação do parser de arquivos .torrent
// =============================================================================

#include "metainfo.hpp"

#include <cstring>
#include <stdexcept>

namespace bt {

// =============================================================================
// FileEntry::path_str
// =============================================================================

std::string FileEntry::path_str() const {
    std::string result;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i > 0) result += '/';
        result += path[i];
    }
    return result;
}

// =============================================================================
// Helpers internos
// =============================================================================

namespace {

/**
 * Localiza o offset e o tamanho do info dict dentro do buffer bencoded bruto.
 *
 * O info_hash é o SHA1 dos bytes brutos do info dict (não do valor decodificado),
 * então precisamos encontrar a posição exata no buffer original.
 *
 * Estratégia: procura a key "4:info" no dict raiz, depois avança um valor
 * bencode completo para determinar o fim do info dict.
 */
std::string_view find_raw_info(std::string_view raw) {
    // Procura pela chave "4:info" no buffer
    // Nota: pode haver falsos positivos, mas em um .torrent bem formado
    // a chave "4:info" aparece apenas uma vez no nível raiz.
    const std::string KEY = "4:info";
    auto key_pos = raw.find(KEY);
    if (key_pos == std::string_view::npos)
        throw std::runtime_error("metainfo: chave 'info' não encontrada");

    size_t value_start = key_pos + KEY.size();
    if (value_start >= raw.size())
        throw std::runtime_error("metainfo: info dict truncado");

    // Avança um valor bencode completo a partir de value_start
    // usando um mini-scanner que conta depth de d...e / l...e
    size_t pos   = value_start;
    int    depth = 0;

    while (pos < raw.size()) {
        char c = raw[pos];

        if (c == 'i') {
            // Inteiro: i<num>e
            ++pos;
            while (pos < raw.size() && raw[pos] != 'e') ++pos;
            ++pos; // consome o 'e'
            if (depth == 0) break;

        } else if (c == 'l' || c == 'd') {
            ++pos;
            ++depth;

        } else if (c == 'e') {
            ++pos;
            --depth;
            if (depth <= 0) break;

        } else if (c >= '0' && c <= '9') {
            // String: <len>:<bytes>
            size_t colon = raw.find(':', pos);
            if (colon == std::string_view::npos)
                throw std::runtime_error("metainfo: string malformada no info dict");

            size_t len = 0;
            for (size_t i = pos; i < colon; ++i) {
                len = len * 10 + (raw[i] - '0');
            }
            pos = colon + 1 + len;
            if (depth == 0) break;

        } else {
            throw std::runtime_error(
                std::string("metainfo: caractere inesperado '") + c + "' ao escanear info dict");
        }
    }

    return raw.substr(value_start, pos - value_start);
}

// -------------------------------------------------------------------------
// Accessors seguros para dicionários bencoded
// -------------------------------------------------------------------------

const BString& require_string(const BDict& d, const std::string& key) {
    auto it = d.find(key);
    if (it == d.end())
        throw std::runtime_error("metainfo: campo obrigatório ausente: '" + key + "'");
    if (!it->second.is_string())
        throw std::runtime_error("metainfo: campo '" + key + "' não é string");
    return it->second.as_string();
}

int64_t require_int(const BDict& d, const std::string& key) {
    auto it = d.find(key);
    if (it == d.end())
        throw std::runtime_error("metainfo: campo obrigatório ausente: '" + key + "'");
    if (!it->second.is_int())
        throw std::runtime_error("metainfo: campo '" + key + "' não é inteiro");
    return it->second.as_int();
}

// -------------------------------------------------------------------------
// Parse dos hashes de peças
// -------------------------------------------------------------------------

std::vector<SHA1::Digest> parse_piece_hashes(const std::string& pieces_raw) {
    if (pieces_raw.size() % 20 != 0)
        throw std::runtime_error(
            "metainfo: campo 'pieces' tem comprimento inválido: " +
            std::to_string(pieces_raw.size()) + " (não é múltiplo de 20)");

    size_t count = pieces_raw.size() / 20;
    std::vector<SHA1::Digest> hashes(count);

    for (size_t i = 0; i < count; ++i) {
        std::memcpy(hashes[i].data(), pieces_raw.data() + i * 20, 20);
    }

    return hashes;
}

// -------------------------------------------------------------------------
// Parse da lista de arquivos (modo multi-file)
// -------------------------------------------------------------------------

std::vector<FileEntry> parse_files(const BList& files_list) {
    std::vector<FileEntry> result;
    result.reserve(files_list.size());
    int64_t offset = 0;

    for (const BValue& file_bv : files_list) {
        if (!file_bv.is_dict())
            throw std::runtime_error("metainfo: entrada de arquivo não é dict");

        const BDict& fd = file_bv.as_dict();
        FileEntry f;

        f.length = require_int(fd, "length");
        f.offset = offset;
        offset  += f.length;

        // path é uma lista de strings (componentes do caminho)
        auto path_it = fd.find("path");
        if (path_it == fd.end() || !path_it->second.is_list())
            throw std::runtime_error("metainfo: arquivo sem 'path'");

        for (const BValue& comp : path_it->second.as_list()) {
            if (!comp.is_string())
                throw std::runtime_error("metainfo: componente de path não é string");
            f.path.push_back(comp.as_string());
        }

        result.push_back(std::move(f));
    }

    return result;
}

} // namespace anônimo

// =============================================================================
// Metainfo::parse — ponto de entrada público
// =============================================================================

Metainfo Metainfo::parse(const std::string& raw) {
    BValue root = decode(raw);
    if (!root.is_dict())
        throw std::runtime_error("metainfo: raiz não é um dicionário");

    const BDict& top = root.as_dict();
    Metainfo m;

    // -------------------------------------------------------------------------
    // Campos do nível raiz
    // -------------------------------------------------------------------------
    m.announce = require_string(top, "announce");

    // announce-list (BEP 12) — opcional
    auto al_it = top.find("announce-list");
    if (al_it != top.end() && al_it->second.is_list()) {
        for (const BValue& tier_bv : al_it->second.as_list()) {
            if (!tier_bv.is_list()) continue;
            std::vector<std::string> tier;
            for (const BValue& url_bv : tier_bv.as_list()) {
                if (url_bv.is_string()) tier.push_back(url_bv.as_string());
            }
            if (!tier.empty()) m.announce_list.push_back(std::move(tier));
        }
    }

    // -------------------------------------------------------------------------
    // info_hash: SHA1 dos bytes brutos do info dict
    // -------------------------------------------------------------------------
    std::string_view raw_info = find_raw_info(raw);
    m.info_hash = SHA1::hash(raw_info.data(), raw_info.size());

    // -------------------------------------------------------------------------
    // Parse do info dict
    // -------------------------------------------------------------------------
    auto info_it = top.find("info");
    if (info_it == top.end() || !info_it->second.is_dict())
        throw std::runtime_error("metainfo: 'info' ausente ou inválido");

    const BDict& info = info_it->second.as_dict();

    m.name         = require_string(info, "name");
    m.piece_length = require_int(info, "piece length");

    m.piece_hashes = parse_piece_hashes(require_string(info, "pieces"));

    // -------------------------------------------------------------------------
    // Single-file vs multi-file
    // -------------------------------------------------------------------------
    auto length_it = info.find("length");
    auto files_it  = info.find("files");

    if (length_it != info.end() && length_it->second.is_int()) {
        // Modo single-file: um único arquivo, path é vazio (usa-se name)
        FileEntry f;
        f.length = length_it->second.as_int();
        f.offset = 0;
        // path vazio — indica single-file ao código consumidor
        m.total_length = f.length;
        m.files.push_back(std::move(f));

    } else if (files_it != info.end() && files_it->second.is_list()) {
        // Modo multi-file
        m.files = parse_files(files_it->second.as_list());
        m.total_length = 0;
        for (const FileEntry& f : m.files) m.total_length += f.length;

    } else {
        throw std::runtime_error(
            "metainfo: info dict não tem 'length' (single-file) nem 'files' (multi-file)");
    }

    // Sanidade: número de peças deve corresponder ao tamanho total
    size_t expected_pieces =
        static_cast<size_t>((m.total_length + m.piece_length - 1) / m.piece_length);
    if (m.piece_hashes.size() != expected_pieces) {
        throw std::runtime_error(
            "metainfo: número de peças inconsistente: esperado " +
            std::to_string(expected_pieces) + ", obtido " +
            std::to_string(m.piece_hashes.size()));
    }

    return m;
}

} // namespace bt
