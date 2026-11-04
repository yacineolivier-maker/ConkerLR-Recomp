// Flou des cinématiques : la profondeur de champ des cinématiques peut être coupée.
// Repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (THIRD_PARTY.md) ; adresses et principe de ce projet, code réécrit ici.
//
// La profondeur de champ est une fonction par image, 0x1C71A0 (appelée en 0x2A5D62). Elle ne dessine que pendant la
// caméra de cinématique ([0x76F0E0] == 1), quand le plan a une mise au point (float [0x84947C] != 0) et son drapeau
// (octet [0x849480]) : la scène réduite en 256x256 et 128x128 est mélangée à l'image selon la profondeur, tout est
// flou aux deux bouts. Réglage : 0 = sans (la fonction est sautée quand elle dessinerait), 1 = celle du jeu.
// Les modes « mis à jour » et « cinématique » de cdetv demandent une profondeur de champ côté hôte : non repris.
#include "cdetv.h"

namespace game {
uint32_t g_flou_cine = 0;
uint32_t g_compte_flou = 0;
}  // namespace game

// entrée de 0x1C71A0 : vrai = retour immédiat (on_true = "ret")
bool FlouCinematique(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    if (game::g_flou_cine != 0) return false;
    bool sans = game::cdetv::R8(0x76F0E0) == 1 && game::cdetv::RF(0x84947C) != 0.0f && game::cdetv::R8(0x849480) != 0;
    if (sans) game::g_compte_flou++;
    return sans;
}
