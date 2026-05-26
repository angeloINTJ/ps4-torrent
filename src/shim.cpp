// shim.cpp — Resolve hidden symbols que não casam entre musl libc.a e libSceLibcInternal.so
// Este arquivo fornece wrappers para símbolos internos da musl que têm visibilidade
// hidden na libc.a mas são implementados em shared objects do SDK do PS4.

#include <arpa/inet.h>

extern "C" {

// musl libc interna — lookup_ipliteral.c chama __inet_aton como hidden symbol.
// A implementação real está em libSceLibcInternal.so, inacessível via hidden ref.
// Fornecemos um wrapper que chama o símbolo público inet_aton.
int __inet_aton(const char* cp, struct in_addr* inp) {
    return inet_aton(cp, inp);
}

}
