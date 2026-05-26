#pragma once
// =============================================================================
// ui/renderer.hpp — Camada de UI para homebrew PS4
//
// O PS4 homebrew tem duas opções de renderização:
//   A) Texto via sceUserService + printf/OrbisKernelDebugOutA  → debug overlay
//   B) SDL2 (portado pela comunidade)                          → UI gráfica completa
//
// Esta implementação usa a abordagem A na camada de "backend" e expõe
// uma API de alto nível orientada a telas (Screen). O módulo ui/sdl_renderer
// (não incluído aqui) pode implementar a mesma interface com SDL2.
//
// A tela principal do app exibe:
//   ┌─────────────────────────────────────────────┐
//   │  PS4 Torrent  v0.1                          │
//   │                                             │
//   │  Arquivo: ubuntu-24.04.iso                  │
//   │  Tamanho: 1.4 GB                            │
//   │                                             │
//   │  [████████████░░░░░░░░] 60.3%               │
//   │  Velocidade: 2.4 MB/s                       │
//   │  Peers: 12  Seeders: 45  Leechers: 78       │
//   │                                             │
//   │  ✓ Concluído / ✗ Cancelado                  │
//   └─────────────────────────────────────────────┘
//
// No PS4, o output vai para o debug log (acessível via ps4debug/uart).
// =============================================================================

#include "../session.hpp"

#include <string>

namespace bt {
namespace ui {

// =============================================================================
// Formatação de valores para display
// =============================================================================

/**
 * Formata bytes em unidade legível (KB, MB, GB).
 * Ex: 1572864 → "1.5 MB"
 */
std::string format_bytes(int64_t bytes);

/**
 * Formata velocidade em bytes/segundo.
 * Ex: 2621440.0 → "2.5 MB/s"
 */
std::string format_speed(double bps);

/**
 * Gera uma barra de progresso ASCII de `width` caracteres.
 * Ex: pct=60.3, width=20 → "[████████████░░░░░░░░]"
 */
std::string progress_bar(float pct, int width = 20);

// =============================================================================
// MainScreen — tela principal do aplicativo
// =============================================================================

/**
 * Renderiza o estado atual do download no console de debug do PS4.
 *
 * No ambiente OpenOrbis, sceKernelDebugOutA (ou printf via musl) grava
 * no UART/PS4debug. Para visualizar em produção, seria necessário SDL2
 * ou a API de vídeo SceVideoOut com OpenGL.
 *
 * @param torrent_name   Nome do arquivo/torrent
 * @param total_size     Tamanho total em bytes
 * @param info           ProgressInfo do callback da Session
 */
void render_progress(
    const std::string& torrent_name,
    int64_t            total_size,
    const ProgressInfo& info
);

/**
 * Exibe mensagem de erro na tela.
 */
void render_error(const std::string& message);

/**
 * Exibe tela de conclusão com resumo do download.
 */
void render_complete(const std::string& torrent_name, int64_t total_bytes, double elapsed_sec);

/**
 * Inicializa o sistema de UserService do PS4 (necessário para input do controle).
 * No-op se chamado em ambiente não-PS4 (para testes no PC).
 */
void init_system_services();

/**
 * Verifica se o botão Circle foi pressionado (cancelar).
 * Retorna true uma vez por pressionamento.
 */
bool check_cancel_button();

} // namespace ui
} // namespace bt
