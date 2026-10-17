// Hooks de diagnostic (config [[hook]]) : journalisent l'état de sous-systèmes du jeu en cours de mise au point.
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

// après XMVDecoder_GetNextFrame (0x2529B6) : code de retour et résultat (1 = nouvelle image)
void LogXmvFrame(X86Context& ctx, uint8_t* base) {
    (void)base;
    static int n = 0;
    if (n++ < 40 || n % 120 == 0)
        LOGI("xmv: GetNextFrame -> %08X résultat %u surface %08X", ctx.eax, kernel::R32(ctx.esp + 0x10),
             kernel::R32(0x779BA0 + 4 * kernel::R32(0x779B9C)));
}


// après l'écriture du terminateur de correctifs (0x3F35D5 -> 0x3F35DB) : ecx = adresse visée
void LogFixupEnd(X86Context& ctx, uint8_t* base) {
    (void)base;
    static int n = 0;
    if (n++ < 6) LOGI("fixup: fin écrite à %08X -> %08X (fixup courant %08X)", ctx.ecx, kernel::R32(ctx.ecx), kernel::R32(0x54A8B0));
}
