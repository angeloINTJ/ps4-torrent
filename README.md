# PS4 Torrent — Homebrew BitTorrent Client para PS4

**Criado por [Ângelo Moisés Alves](https://github.com/angeloINTJ)**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![GitHub repo](https://img.shields.io/badge/repo-angeloINTJ%2Fps4--torrent-green.svg)](https://github.com/angeloINTJ/ps4-torrent)

Cliente BitTorrent **open source** nativo para PS4 jailbroken, desenvolvido com o
[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
Implementa o protocolo BitTorrent (BEP 3) do zero em C++17, sem dependências externas além do musl libc.

---

## Pré-requisitos

### No PS4

| Requisito | Versão | Observação |
|---|---|---|
| PS4 com jailbreak | FW ≤ 11.00 | GoldHEN ou ps4debug ativo |
| Acesso FTP | — | Para enviar arquivos `.torrent` |
| Remote PKG Installer | — | Para instalar o homebrew |

### No PC (ambiente de build)

| Requisito | Versão | Como instalar |
|---|---|---|
| Ubuntu / Debian | 20.04+ | Ou WSL2 no Windows |
| Clang/LLVM | ≥ 12 | `sudo apt install clang-18 lld-18` |
| OpenOrbis Toolchain | ≥ 0.5.2 | [Download](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/tag/v0.5.4) |
| OpenSSL 1.1 | 1.1.x | Necessário para o `PkgTool.Core` (empacotamento) |

---

## Build

### 1. Instalar Clang e LLD

```bash
sudo apt install -y clang-18 lld-18
```

### 2. Instalar o OpenOrbis SDK

```bash
# Baixa e extrai o toolchain
wget https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
sudo mkdir -p /opt/openorbis
sudo tar xzf toolchain-llvm-18.tar.gz -C /opt/openorbis/
```

O SDK será extraído em `/opt/openorbis/OpenOrbis/PS4Toolchain/`.
Aponte a variável `OO_PS4_TOOLCHAIN` para esse diretório.

### 3. Instalar OpenSSL 1.1 (para o PkgTool.Core)

O `PkgTool.Core` que vem com o toolchain depende do OpenSSL 1.1.
No Ubuntu 24.04+ o pacote `libssl1.1` foi removido — é necessário compilar:

```bash
wget https://www.openssl.org/source/openssl-1.1.1w.tar.gz
tar xzf openssl-1.1.1w.tar.gz
cd openssl-1.1.1w
./Configure --prefix=$HOME/openssl1.1 --openssldir=$HOME/openssl1.1/ssl shared no-tests linux-x86_64
make -j$(nproc)
mkdir -p $HOME/openssl1.1/lib
cp libssl.so.1.1 libcrypto.so.1.1 $HOME/openssl1.1/lib/
```

### 4. Configurar variáveis de ambiente

Adicione ao seu `~/.bashrc`:

```bash
export OO_PS4_TOOLCHAIN=/opt/openorbis/OpenOrbis/PS4Toolchain
export PATH="$HOME/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/openssl1.1/lib:$LD_LIBRARY_PATH"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
```

Crie symlinks para o `clang` e `ld.lld`:

```bash
mkdir -p $HOME/bin
ln -sf /usr/bin/clang-18 $HOME/bin/clang
ln -sf /usr/bin/clang++-18 $HOME/bin/clang++
ln -sf /usr/bin/lld-18 $HOME/bin/ld.lld
```

### 5. Clonar e compilar

```bash
git clone https://github.com/angeloINTJ/ps4-torrent
cd ps4-torrent

# Gera os assets (param.sfo + icon0.png)
./scripts/setup_assets.sh

# Build completo
make
```

O PKG será gerado em:
```
output/UP0000-BTRC00001_00-0000000000000000.pkg
```

### 6. Instalar no PS4

1. Coloque um arquivo `.torrent` em `/data/pkg/torrents/` via FTP (ps4debug)
2. Instale o PKG pelo Remote PKG Installer
3. Lance o app pelo XMB

O download será salvo em `/data/pkg/downloads/`.

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
│   ├── shim.cpp            — Compatibilidade musl <-> PS4 (hidden symbols)
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

Pull requests são bem-vindos! O projeto é open source (MIT) — fique à vontade
para contribuir, abrir issues ou fazer forks. Prioridade atual:

1. **SDL2 UI** — renderização gráfica no framebuffer do PS4
2. **UDP Tracker** (BEP 15)
3. **DHT** (BEP 5) — download sem tracker central
4. **Seletor de .torrent** via controle

---

## Licença

Este projeto é **open source** sob a licença [MIT](LICENSE).

Copyright (c) 2026 **Ângelo Moisés Alves**

Você é livre para usar, modificar e distribuir este software, desde que mantenha
os créditos ao autor original.

---

## Aviso legal

Este software é para fins educacionais e de pesquisa.
O uso para download de conteúdo protegido por direitos autorais é de responsabilidade do usuário.
O OpenOrbis Toolchain não usa nenhuma ferramenta ou código proprietário da Sony.
