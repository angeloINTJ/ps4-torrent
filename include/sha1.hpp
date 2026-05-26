#pragma once
// =============================================================================
// sha1.hpp — SHA1 header-only, baseada no RFC 3174
//
// Sem dependências externas. Funciona com o musl libc do OpenOrbis.
// Uso:
//   auto digest = bt::SHA1::hash(data, len);
//   // ou incremental:
//   bt::SHA1 ctx;
//   ctx.update(ptr, size);
//   auto d = ctx.finalize();
// =============================================================================

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace bt {

class SHA1 {
public:
    // SHA1 produz 160 bits = 20 bytes
    static constexpr size_t DIGEST_SIZE = 20;
    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    SHA1() noexcept { reset(); }

    // -------------------------------------------------------------------------
    // Reinicia o contexto para reutilização
    // -------------------------------------------------------------------------
    void reset() noexcept {
        count_    = 0;
        buf_used_ = 0;
        // Valores iniciais definidos pelo padrão SHA1
        state_[0] = 0x67452301u;
        state_[1] = 0xEFCDAB89u;
        state_[2] = 0x98BADCFEu;
        state_[3] = 0x10325476u;
        state_[4] = 0xC3D2E1F0u;
    }

    // -------------------------------------------------------------------------
    // Alimenta bytes ao contexto (pode ser chamado múltiplas vezes)
    // -------------------------------------------------------------------------
    void update(const void* data, size_t len) noexcept {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        count_ += len;

        // Completa um bloco parcial se houver bytes no buffer interno
        if (buf_used_ > 0) {
            size_t space = BLOCK_SIZE - buf_used_;
            size_t fill  = (len < space) ? len : space;
            std::memcpy(buf_ + buf_used_, ptr, fill);
            buf_used_ += fill;
            ptr        += fill;
            len        -= fill;

            if (buf_used_ == BLOCK_SIZE) {
                compress(buf_);
                buf_used_ = 0;
            }
        }

        // Processa blocos completos diretamente do input (zero-copy)
        while (len >= BLOCK_SIZE) {
            compress(ptr);
            ptr += BLOCK_SIZE;
            len -= BLOCK_SIZE;
        }

        // Salva bytes restantes no buffer
        if (len > 0) {
            std::memcpy(buf_, ptr, len);
            buf_used_ = len;
        }
    }

    // -------------------------------------------------------------------------
    // Finaliza e retorna o digest. NÃO reutilize o contexto após esta chamada
    // sem chamar reset() primeiro.
    // -------------------------------------------------------------------------
    Digest finalize() noexcept {
        // Padding: bit 1 seguido de zeros até 56 mod 64 bytes, depois tamanho em bits
        uint64_t bit_count_be = to_big_endian_64(count_ * 8);

        uint8_t pad = 0x80;
        update(&pad, 1);

        // Preenche até restar 8 bytes no bloco (para o bit count)
        uint8_t zero = 0;
        while (buf_used_ != 56) update(&zero, 1);

        // Anexa tamanho em bits (big-endian, 8 bytes)
        update(&bit_count_be, 8);

        // Serializa estado em big-endian
        Digest d;
        for (int i = 0; i < 5; ++i) {
            d[i * 4 + 0] = (state_[i] >> 24) & 0xFF;
            d[i * 4 + 1] = (state_[i] >> 16) & 0xFF;
            d[i * 4 + 2] = (state_[i] >>  8) & 0xFF;
            d[i * 4 + 3] = (state_[i] >>  0) & 0xFF;
        }
        return d;
    }

    // -------------------------------------------------------------------------
    // One-shot: hash de dados em memória
    // -------------------------------------------------------------------------
    static Digest hash(const void* data, size_t len) noexcept {
        SHA1 ctx;
        ctx.update(data, len);
        return ctx.finalize();
    }

    static Digest hash(const std::string& s) noexcept {
        return hash(s.data(), s.size());
    }

    // Compara dois digests de forma segura (sem early-exit para evitar timing attacks)
    static bool equal(const Digest& a, const Digest& b) noexcept {
        uint8_t diff = 0;
        for (size_t i = 0; i < DIGEST_SIZE; ++i) diff |= a[i] ^ b[i];
        return diff == 0;
    }

private:
    static constexpr size_t BLOCK_SIZE = 64;

    uint32_t state_[5];
    uint64_t count_;        // Total de bytes processados
    uint8_t  buf_[BLOCK_SIZE];
    size_t   buf_used_;

    // Rotação circular à esquerda de 32 bits
    static uint32_t rotl(uint32_t v, int n) noexcept {
        return (v << n) | (v >> (32 - n));
    }

    static uint64_t to_big_endian_64(uint64_t v) noexcept {
        // Troca bytes se necessário (PS4 é little-endian x86_64)
        return ((v & 0x00000000000000FFull) << 56) |
               ((v & 0x000000000000FF00ull) << 40) |
               ((v & 0x0000000000FF0000ull) << 24) |
               ((v & 0x00000000FF000000ull) <<  8) |
               ((v & 0x000000FF00000000ull) >>  8) |
               ((v & 0x0000FF0000000000ull) >> 24) |
               ((v & 0x00FF000000000000ull) >> 40) |
               ((v & 0xFF00000000000000ull) >> 56);
    }

    // -------------------------------------------------------------------------
    // Compressão SHA1 de um bloco de 64 bytes
    // -------------------------------------------------------------------------
    void compress(const uint8_t* block) noexcept {
        // Expande os 16 words do bloco para 80
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) <<  8) |
                   (static_cast<uint32_t>(block[i * 4 + 3]) <<  0);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2],
                 d = state_[3], e = state_[4];

        // Quatro rodadas de 20 operações cada
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }

            uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = tmp;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }
};

} // namespace bt
