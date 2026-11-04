// Sons coupés après une pause : la durée des sons en boucle et minutés ne s'use plus pendant le menu de pause.
// Repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (THIRD_PARTY.md) ; adresses et principe de ce projet, code réécrit ici.
//
// Un émetteur en boucle (mise à jour 0x30730) décompte [+0x48]+4, un émetteur minuté (0x30BC0) son +0x48, tous deux
// des ticks 60 Hz de l'image [0x7FA208]. La pause du jeu [0x7FA275] gèle le temps du jeu mais pas ce compte : un
// long son finit trop tôt après quelques pauses (le bip du missile du boss d'It's War), ou ne revient pas. Le jeu
// d'origine fait sans doute de même : c'est une amélioration. Pendant la pause, le décompte reçoit d'avance les
// ticks que la mise à jour va lui retirer.
#include "cdetv.h"

namespace game {
bool g_sons_pause = true;
uint32_t g_compte_sons = 0;
uint32_t g_appels_sons_pause = 0;  // mises à jour d'émetteurs vues pendant la pause
}  // namespace game

// entrée de 0x30730 (émetteur en boucle, thiscall : ecx = émetteur)
void SonsPauseBoucle(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (game::cdetv::R8(0x7FA275)) game::g_appels_sons_pause++;
    if (!game::g_sons_pause || !game::cdetv::R8(0x7FA275) || !game::cdetv::Ok(ctx.ecx + 0x48)) return;
    uint32_t desc = kernel::R32(ctx.ecx + 0x48);
    if (game::cdetv::Ok(desc + 4) && int32_t(kernel::R32(desc + 4)) >= 1) {
        kernel::W32(desc + 4, kernel::R32(desc + 4) + kernel::R32(0x7FA208));
        game::g_compte_sons++;
    }
}

// entrée de 0x30BC0 (émetteur minuté, thiscall : ecx = émetteur)
void SonsPauseMinute(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (game::cdetv::R8(0x7FA275)) game::g_appels_sons_pause++;
    if (!game::g_sons_pause || !game::cdetv::R8(0x7FA275) || !game::cdetv::Ok(ctx.ecx + 0x48)) return;
    if (int32_t(kernel::R32(ctx.ecx + 0x48)) >= 1) {
        kernel::W32(ctx.ecx + 0x48, kernel::R32(ctx.ecx + 0x48) + kernel::R32(0x7FA208));
        game::g_compte_sons++;
    }
}
