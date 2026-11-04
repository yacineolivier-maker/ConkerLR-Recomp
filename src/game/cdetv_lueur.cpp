// Réglage de la lueur : intensité de la lueur du jeu (brume et éblouissement), de 0 à 100 %.
// Repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (THIRD_PARTY.md) ; adresses et principe de ce projet, code réécrit ici.
//
// La lueur (0x1D95D0, une fois par image) ajoute deux lumières à l'image : la brume (0x1D9790, l'image réduite
// redessinée par-dessus, alpha des sommets, mélange SRC_ALPHA/ONE ; DrawVerticesUP appelé en 0x1D9C6E) et
// l'éblouissement (0x1DA3F0, texture x TFACTOR blanc, mélange ONE/ONE ; TFACTOR posé en 0x1DA60B). Les deux sont
// linéaires dans ces valeurs : on les met à l'échelle. 100 % = le jeu tel quel.
#include "cdetv.h"

namespace game {
uint32_t g_lueur = 100;
}  // namespace game

namespace {
float Echelle() { return game::g_lueur >= 100 ? 1.0f : float(game::g_lueur) / 100.0f; }
}  // namespace

// avant l'appel de DrawVerticesUP en 0x1D9C6E ([esp] type, [esp+4] nombre, [esp+8] sommets) : 4 sommets de 0x18
// octets, D3DCOLOR en +0x0C dont seul l'alpha compte
void LueurBrume(X86Context& ctx, uint8_t* base) {
    (void)base;
    float k = Echelle();
    if (k >= 1.0f) return;
    uint32_t data = kernel::R32(ctx.esp + 8);
    if (!game::cdetv::Ok(data, 4 * 0x18)) return;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t va = data + i * 0x18 + 0x0C, c = kernel::R32(va);
        uint32_t a = uint32_t(float(c >> 24) * k + 0.5f);
        kernel::W32(va, (c & 0x00FFFFFF) | a << 24);
    }
}

// avant l'appel de SetRenderState_TextureFactor en 0x1DA60B (valeur en [esp]) : blanc ramené au gris
void LueurEblouissement(X86Context& ctx, uint8_t* base) {
    (void)base;
    float k = Echelle();
    if (k >= 1.0f) return;
    uint32_t g = uint32_t(255.0f * k + 0.5f);
    kernel::W32(ctx.esp, 0xFF000000u | g << 16 | g << 8 | g);
}
