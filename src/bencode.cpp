// =============================================================================
// bencode.cpp — Implementação do parser e encoder bencode
// =============================================================================

#include "bencode.hpp"

#include <charconv>   // std::from_chars — sem alocação, seguro em PS4/musl
#include <cstring>
#include <stdexcept>

namespace bt {

// =============================================================================
// Parser interno — estado de parsing encapsulado em struct
// =============================================================================

namespace {

struct Parser {
    const char* begin;
    const char* end;
    const char* cur;

    explicit Parser(std::string_view sv)
        : begin(sv.data()), end(sv.data() + sv.size()), cur(sv.data()) {}

    // -------------------------------------------------------------------------
    // Primitivas de navegação
    // -------------------------------------------------------------------------

    bool at_end() const noexcept { return cur >= end; }

    char peek() const {
        if (at_end())
            throw std::runtime_error("bencode: fim inesperado da entrada");
        return *cur;
    }

    char consume() {
        char c = peek();
        ++cur;
        return c;
    }

    void expect(char c) {
        char got = consume();
        if (got != c) {
            throw std::runtime_error(
                std::string("bencode: esperado '") + c + "', obtido '" + got + "'");
        }
    }

    // -------------------------------------------------------------------------
    // Parsers por tipo
    // -------------------------------------------------------------------------

    BValue parse();

    BValue parse_integer() {
        expect('i');

        const char* num_start = cur;
        while (!at_end() && *cur != 'e') ++cur;
        const char* num_end = cur;
        expect('e');

        BInt result = 0;
        auto [ptr, ec] = std::from_chars(num_start, num_end, result);
        if (ec != std::errc{} || ptr != num_end)
            throw std::runtime_error("bencode: inteiro inválido");

        return BValue(result);
    }

    BString parse_string() {
        // Lê o comprimento decimal
        const char* len_start = cur;
        while (!at_end() && *cur != ':') ++cur;
        const char* len_end = cur;
        expect(':');

        size_t len = 0;
        auto [ptr, ec] = std::from_chars(len_start, len_end, len);
        if (ec != std::errc{})
            throw std::runtime_error("bencode: comprimento de string inválido");

        if (static_cast<size_t>(end - cur) < len)
            throw std::runtime_error("bencode: string excede o buffer");

        BString s(cur, len);
        cur += len;
        return s;
    }

    BList parse_list() {
        expect('l');
        BList result;
        while (peek() != 'e') {
            result.push_back(parse());
        }
        expect('e');
        return result;
    }

    BDict parse_dict() {
        expect('d');
        BDict result;
        while (peek() != 'e') {
            // Keys em bencode são sempre strings
            BString key = parse_string();
            BValue  val = parse();
            result.emplace(std::move(key), std::move(val));
        }
        expect('e');
        return result;
    }
};

BValue Parser::parse() {
    char c = peek();

    if (c == 'i')                    return parse_integer();
    if (c == 'l')                    return BValue(parse_list());
    if (c == 'd')                    return BValue(parse_dict());
    if (c >= '0' && c <= '9')        return BValue(parse_string());

    throw std::runtime_error(
        std::string("bencode: caractere inesperado '") + c + "' na posição " +
        std::to_string(cur - begin));
}

} // namespace anônimo

// =============================================================================
// API pública
// =============================================================================

BValue decode(std::string_view data) {
    Parser p(data);
    BValue result = p.parse();
    return result;
}

void encode_into(const BValue& val, std::string& out) {
    if (val.is_int()) {
        out += 'i';
        out += std::to_string(val.as_int());
        out += 'e';
        return;
    }

    if (val.is_string()) {
        const BString& s = val.as_string();
        out += std::to_string(s.size());
        out += ':';
        out += s;
        return;
    }

    if (val.is_list()) {
        out += 'l';
        for (const BValue& item : val.as_list()) encode_into(item, out);
        out += 'e';
        return;
    }

    if (val.is_dict()) {
        out += 'd';
        // std::map itera em ordem lexicográfica — correto para bencode
        for (const auto& [k, v] : val.as_dict()) {
            out += std::to_string(k.size());
            out += ':';
            out += k;
            encode_into(v, out);
        }
        out += 'e';
        return;
    }

    throw std::runtime_error("bencode: tipo de valor desconhecido");
}

std::string encode(const BValue& val) {
    std::string out;
    out.reserve(64);
    encode_into(val, out);
    return out;
}

} // namespace bt
