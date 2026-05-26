#pragma once
// =============================================================================
// bencode.hpp — Decoder e encoder do formato Bencode (BEP 3)
//
// Bencode é o formato de serialização usado em arquivos .torrent e no
// protocolo tracker. Suporta quatro tipos:
//   - Inteiro:     i<número>e          → ex: i42e
//   - String:      <len>:<bytes>       → ex: 4:spam
//   - Lista:       l<items>e           → ex: li1e4:spame
//   - Dicionário:  d<key-value...>e    → ex: d3:fooi1ee  (keys sempre strings)
// =============================================================================

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace bt {

// Declaração forward para o tipo recursivo
struct BValue;

using BInt    = int64_t;
using BString = std::string;
using BList   = std::vector<BValue>;
using BDict   = std::map<BString, BValue>;   // Map ordenado — bencode exige keys em ordem

/**
 * Union tipada que representa qualquer valor bencode.
 *
 * Usa std::variant para acesso type-safe sem alocação extra.
 * Para criar: BValue v = BString("hello");
 * Para ler:   v.as_string()  ou  std::get<BString>(v.data)
 */
struct BValue {
    std::variant<BInt, BString, BList, BDict> data;

    // -------------------------------------------------------------------------
    // Construtores
    // -------------------------------------------------------------------------
    BValue() = default;
    explicit BValue(BInt v)    : data(v)             {}
    explicit BValue(BString v) : data(std::move(v))  {}
    explicit BValue(BList v)   : data(std::move(v))  {}
    explicit BValue(BDict v)   : data(std::move(v))  {}

    // Conveniência para literais inteiros
    explicit BValue(int v)      : data(static_cast<BInt>(v)) {}

    // -------------------------------------------------------------------------
    // Predicados de tipo
    // -------------------------------------------------------------------------
    bool is_int()    const noexcept { return std::holds_alternative<BInt>(data);    }
    bool is_string() const noexcept { return std::holds_alternative<BString>(data); }
    bool is_list()   const noexcept { return std::holds_alternative<BList>(data);   }
    bool is_dict()   const noexcept { return std::holds_alternative<BDict>(data);   }

    // -------------------------------------------------------------------------
    // Accessors — lançam std::bad_variant_access se o tipo não bater
    // -------------------------------------------------------------------------
    BInt&          as_int()    { return std::get<BInt>(data);    }
    BString&       as_string() { return std::get<BString>(data); }
    BList&         as_list()   { return std::get<BList>(data);   }
    BDict&         as_dict()   { return std::get<BDict>(data);   }

    const BInt&    as_int()    const { return std::get<BInt>(data);    }
    const BString& as_string() const { return std::get<BString>(data); }
    const BList&   as_list()   const { return std::get<BList>(data);   }
    const BDict&   as_dict()   const { return std::get<BDict>(data);   }
};

// =============================================================================
// API pública
// =============================================================================

/**
 * Decodifica um buffer bencoded e retorna o BValue raiz.
 *
 * @param data   Buffer de entrada (pode conter bytes nulos — usa string_view)
 * @throws std::runtime_error   em entrada malformada
 */
BValue decode(std::string_view data);

/**
 * Codifica um BValue para bytes bencode.
 * Dicionários são emitidos com keys em ordem lexicográfica (garantido por std::map).
 */
std::string encode(const BValue& val);

/**
 * Versão de encode que grava em um buffer existente (evita alocação extra).
 */
void encode_into(const BValue& val, std::string& out);

} // namespace bt
