# PS4 Torrent — Homebrew BitTorrent Client para PS4

Cliente BitTorrent nativo para PS4 jailbroken, desenvolvido com o
[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
Implementa o protocolo BitTorrent (BEP 3) do zero em C++17, sem dependências externas além do musl libc.

---

## Pré-requisitos

| Requisito | Versão | Observação |
|---|---|---|
| PS4 com jailbreak | FW ≤ 11.00 | GoldHEN ou ps4debug ativo |
| OpenOrbis Toolchain | ≥ 0.5.2 | [Download](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases) |
| Clang/LLVM | ≥ 12 | Vem com o toolchain |
| `ld.lld` | ≥ 12 | Vem com o toolchain |
| PkgTool.Core | Qualquer | Vem com o toolchain |

---

## Build

```bash
# 1. Instala o OpenOrbis e exporta a variável de ambiente
export OO_PS4_TOOLCHAIN=/opt/openorbis   # ou onde instalou

# 2. Clona e compila
git clone https://github.com/seuusuario/ps4-torrent
cd ps4-torrent
make

# O PKG estará em:
#   output/ps4-torrent.pkg
```

### Instalar no PS4

1. Coloque um arquivo `.torrent` em `/data/pkg/torrents/` via FTP (ps4debug)
2. Instale o PKG pelo Remote PKG Installer
3. Lance o app pelo XMB

---

## Arquitetura

```
ps4-torrent/
├── include/
│   ├── bencode.hpp        — Parser/encoder do formato Bencode (BEP 3)
│   ├── sha1.hpp           — SHA1 header-only (RFC 3174), sem dependências
│   ├── metainfo.hpp       — Parser de arquivos .torrent
│   ├── tracker.hpp        — Cliente HTTP para announce de tracker
│   ├── peer_wire.hpp      — Protocolo wire BitTorrent (handshake, mensagens)
│   ├── piece_manager.hpp  — Gerenciador de peças, SHA1 verify, escrita em disco
│   ├── session.hpp        — Orquestrador: tracker + pool de peers + progresso
│   └── ui/
│       └── renderer.hpp   — Camada de UI (debug log + input do controle)
│
├── src/
│   ├── bencode.cpp
│   ├── metainfo.cpp
│   ├── tracker.cpp
│   ├── peer_wire.cpp
│   ├── piece_manager.cpp
│   ├── session.cpp
│   ├── ui/
│   │   └── renderer.cpp
│   └── main.cpp           — Ponto de entrada (eboot)
│
├── assets/
│   ├── param.sfo          — Metadados do app (gerado pelo toolchain)
│   └── icon0.png          — Ícone 512×512 (opcional)
│
└── Makefile
```

### Fluxo de dados

```
main.cpp
  └─► Session::start()
        ├─► Metainfo::parse()          — lê e parseia o .torrent
        ├─► tracker_announce()         — obtém lista de peers
        ├─► PieceManager (construtor)  — aloca arquivos no disco
        ├─► [Thread] tracker_thread()  — reannounce periódico
        ├─► [Thread] progress_thread() — dispara ProgressCallback
        └─► run_peer_pool()
              └─► [Thread × N] peer_worker()
                    ├─► PeerConnection::connect()
                    ├─► PeerConnection::handshake()
                    └─► loop:
                          ├─► PeerConnection::read_message()
                          ├─► PieceManager::next_request()
                          ├─► PeerConnection::send_request()
                          └─► PieceManager::receive_block()
                                ├─► SHA1::hash()      — verifica peça
                                └─► write_piece()     — grava no disco
```

---

## Limitações conhecidas (v0.1)

| Limitação | BEP relacionado | Plano |
|---|---|---|
| Apenas HTTP tracker (não HTTPS nem UDP) | BEP 15 | v0.2 |
| Sem DHT (Distributed Hash Table) | BEP 5 | v0.3 |
| Sem PEX (Peer Exchange) | BEP 11 | v0.3 |
| Sem seeding (apenas download) | BEP 3 | v0.2 |
| UI apenas via debug log (sem gráfico) | — | v0.2 (SDL2) |
| Seleção de arquivo .torrent manual | — | v0.2 |
| Apenas o primeiro .torrent do diretório | — | v0.2 |

---

## Estrutura do protocolo BitTorrent implementada

```
BEP 3  — Protocolo base        ✓ Completo
  ├── Bencode                  ✓ decode + encode
  ├── Metainfo (.torrent)      ✓ single-file e multi-file
  ├── Tracker HTTP announce    ✓ compact peers
  ├── Peer wire protocol       ✓ todas as mensagens do protocolo base
  │     ├── Handshake          ✓
  │     ├── Choke/Unchoke      ✓
  │     ├── Interested         ✓
  │     ├── Have/Bitfield      ✓
  │     ├── Request/Piece      ✓
  │     └── Cancel             ✓
  └── Verificação SHA1         ✓ por peça
```

---

## Contribuindo

Pull requests bem-vindos. Prioridade atual:

1. **SDL2 UI** — renderização gráfica no framebuffer do PS4
2. **UDP Tracker** (BEP 15)
3. **DHT** (BEP 5) — download sem tracker central
4. **Seletor de .torrent** via controle

---

## Aviso legal

Este software é para fins educacionais e de pesquisa.
O uso para download de conteúdo protegido por direitos autorais é de responsabilidade do usuário.
O OpenOrbis Toolchain não usa nenhuma ferramenta ou código proprietário da Sony.
