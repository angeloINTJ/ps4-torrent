// =============================================================================
// ui/renderer.cpp — Implementação da UI para PS4 homebrew
//
// Usa sceUserService e scePad para input, e OrbisKernelDebugOutA
// (mapeado via include do OpenOrbis) para output no debug log.
// =============================================================================

#include "ui/renderer.hpp"

// Cabeçalhos PS4 via OpenOrbis
// Estes arquivos existem em $OO_PS4_TOOLCHAIN/include/
#include <orbis/UserService.h>
#include <orbis/Pad.h>

#include <cstdio>
#include <cstring>
#include <cmath>

// No OpenOrbis, printf() grava no OrbisKernelDebugOutA automaticamente
// via musl + libkernel hook quando rodando com ps4debug ativo.
#define PS4_LOG(...) printf(__VA_ARGS__)

namespace bt {
namespace ui {

// =============================================================================
// Estado do sistema de input
// =============================================================================

static int32_t  g_user_id    = -1;
static int32_t  g_pad_handle = -1;
static bool     g_services_ok = false;

// =============================================================================
// init_system_services
// =============================================================================

void init_system_services() {
    // Inicializa o UserService (necessário para obter o userId para o Pad)
    int32_t ret = sceUserServiceInitialize(nullptr);
    if (ret != 0) {
        PS4_LOG("[ui] sceUserServiceInitialize falhou: 0x%08X\n", ret);
        return;
    }

    // Obtém o ID do usuário inicial (primeiro usuário logado)
    ret = sceUserServiceGetInitialUser(&g_user_id);
    if (ret != 0) {
        PS4_LOG("[ui] sceUserServiceGetInitialUser falhou: 0x%08X\n", ret);
        return;
    }

    // Inicializa o módulo de Pad (controle)
    ret = scePadInit();
    if (ret != 0) {
        PS4_LOG("[ui] scePadInit falhou: 0x%08X\n", ret);
        return;
    }

    // Abre o controle do usuário (tipo 0 = DualShock 4)
    g_pad_handle = scePadOpen(g_user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (g_pad_handle < 0) {
        PS4_LOG("[ui] scePadOpen falhou: 0x%08X\n", g_pad_handle);
        return;
    }

    g_services_ok = true;
    PS4_LOG("[ui] Serviços do sistema inicializados. UserID=%d PadHandle=%d\n",
            g_user_id, g_pad_handle);
}

// =============================================================================
// check_cancel_button — verifica pressionamento do botão Circle (O)
// =============================================================================

bool check_cancel_button() {
    if (!g_services_ok || g_pad_handle < 0) return false;

    OrbisPadData pad_data{};
    int32_t ret = scePadReadState(g_pad_handle, &pad_data);
    if (ret != 0) return false;

    // ORBIS_PAD_BUTTON_CIRCLE = botão O (cancelar/voltar no layout JP/BR)
    return (pad_data.buttons & ORBIS_PAD_BUTTON_CIRCLE) != 0;
}

// =============================================================================
// Formatação
// =============================================================================

std::string format_bytes(int64_t bytes) {
    char buf[32];
    if (bytes < 1024LL) {
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    } else if (bytes < 1024LL * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

std::string format_speed(double bps) {
    return format_bytes(static_cast<int64_t>(bps)) + "/s";
}

std::string progress_bar(float pct, int width) {
    if (pct < 0.0f)   pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    int filled = static_cast<int>(std::round(pct / 100.0f * width));

    std::string bar = "[";
    for (int i = 0; i < width; ++i) {
        bar += (i < filled) ? '#' : '.';
    }
    bar += ']';
    return bar;
}

// =============================================================================
// render_progress
// =============================================================================

void render_progress(
    const std::string& torrent_name,
    int64_t            total_size,
    const ProgressInfo& info)
{
    // Limpa a tela de debug emitindo várias linhas em branco
    // (em UART/ps4debug não há escape codes de terminal garantidos)
    PS4_LOG("\n\n\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  PS4 Torrent v0.1\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  Arquivo  : %s\n", torrent_name.c_str());
    PS4_LOG("  Tamanho  : %s\n", format_bytes(total_size).c_str());
    PS4_LOG("\n");
    PS4_LOG("  %s %.1f%%\n", progress_bar(info.pct).c_str(), info.pct);
    PS4_LOG("  Baixados : %s / %s\n",
            format_bytes(info.bytes_done).c_str(),
            format_bytes(info.bytes_total).c_str());
    PS4_LOG("  Velocidade: %s\n", format_speed(info.speed_bps).c_str());
    PS4_LOG("  Peers    : %d  Seeders: %d  Leechers: %d\n",
            info.peers_active, info.seeders, info.leechers);
    PS4_LOG("\n");
    PS4_LOG("  [O] Cancelar\n");
    PS4_LOG("============================================================\n");
}

// =============================================================================
// render_error
// =============================================================================

void render_error(const std::string& message) {
    PS4_LOG("\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  PS4 Torrent v0.1  --  ERRO\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  %s\n", message.c_str());
    PS4_LOG("============================================================\n");
}

// =============================================================================
// render_complete
// =============================================================================

void render_complete(const std::string& torrent_name, int64_t total_bytes, double elapsed_sec) {
    double avg_speed = (elapsed_sec > 0) ? (total_bytes / elapsed_sec) : 0.0;

    PS4_LOG("\n\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  PS4 Torrent v0.1  --  CONCLUIDO!\n");
    PS4_LOG("============================================================\n");
    PS4_LOG("  Arquivo  : %s\n", torrent_name.c_str());
    PS4_LOG("  Total    : %s\n", format_bytes(total_bytes).c_str());
    PS4_LOG("  Tempo    : %.0f segundos\n", elapsed_sec);
    PS4_LOG("  Velocidade media: %s\n", format_speed(avg_speed).c_str());
    PS4_LOG("\n");
    PS4_LOG("  Pressione [X] para sair.\n");
    PS4_LOG("============================================================\n");
}

} // namespace ui
} // namespace bt
