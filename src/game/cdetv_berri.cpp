// Berri dans le sol : couchée, elle n'est plus basculée tête en bas à travers le sol (arène finale, après le tir).
// Repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (THIRD_PARTY.md) ; adresses et principe de ce projet, code réécrit ici.
//
// La matrice d'un personnage est refaite avant chaque dessin à partir de trois angles en degrés (0x808F0,
// [esp+4] = index du personnage ; x en +0x1C0). La cinématique couche Berri en jouant sa pose debout avec x = -90,
// puis passe à « deadpose2 », déjà couchée, en laissant x à -90 : la pose couchée est basculée une seconde fois,
// tête dans le sol. Le jeu d'origine fait de même (amélioration). Tant que son animation est une « deadpose »,
// x = -90 est dessiné comme 0. Nom de l'animation : entrée +0xA4 de sa table [[perso+0x74]+0x50], 12 octets chacune.
#include <string>

#include "cdetv.h"
#include "core/log.h"

namespace game {
bool g_berri_sol = true;
}  // namespace game

// entrée de 0x808F0 (matrice d'un personnage)
void BerriSol(X86Context& ctx, uint8_t* base) {
    (void)base;
    using game::cdetv::Ok;
    if (!game::g_berri_sol) return;
    uint32_t persos = kernel::R32(0x84A5F8), idx = kernel::R32(ctx.esp + 4);
    if (idx > 0x80 || !Ok(persos)) return;
    uint32_t c = persos + idx * 0x6D0;
    if (!Ok(c + 0x1C0) || kernel::R32(c + 0x68) != 0x97 || kernel::R32(c + 0x1C0) != 0xC2B40000u) return;  // -90.0f
    uint32_t anim = kernel::R32(c + 0xA4), modele = kernel::R32(c + 0x74);
    if (anim >= 0x100 || !Ok(modele + 0x50)) return;
    uint32_t table = kernel::R32(modele + 0x50);
    if (!Ok(table + anim * 12)) return;
    uint32_t nom = kernel::R32(table + anim * 12);
    if (!Ok(nom)) return;
    std::string s = kernel::ReadCString(nom, 96);
    if (s.find("berimatrix\\deadpose") == std::string::npos) return;
    kernel::W32(c + 0x1C0, 0);  // la cinématique le remet à chaque image
    static int n = 0;
    if (n++ % 300 == 0) LOGI("berri: « %s » basculée de -90 : dessinée à plat", s.c_str());
}
