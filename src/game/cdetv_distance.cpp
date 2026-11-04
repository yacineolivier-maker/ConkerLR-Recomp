// Disparitions au loin : herbe, fleurs, personnages et fourrure gardent tout leur détail à distance.
// Repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (THIRD_PARTY.md) ; adresses et principe de ce projet, code réécrit ici.
//
// Herbe : 0x1A9540 (cdecl : touffe, short joueur) mesure le centre de la touffe (+0x2C/+0x30/+0x34) à la caméra du
// joueur ([[0x8470DC] + joueur * 0x770 + 0x3E0] + 0x78) : jusqu'à 800 tout, de 800 à 2500 de moins en moins de brins
// (et immobiles), au-delà rien. Les deux limites sont des constantes partagées : on pose plutôt le centre sur la
// caméra pour le test, et on le remet à la lecture suivante, la translation D3DX 0x4288F5 appelée en 0x1A969F.
// Toutes les touffes passent alors par le programme ondulant, dont la « poussée » vers la caméra (c[101], posée en
// 0x1A952C) mélange coordonnées locales et monde : elle est mise à zéro.
// Brouillard : l'herbe ne reçoit jamais la brume du niveau (aucun des deux programmes n'écrit oFog), invisible tant
// qu'elle s'arrêtait à 800 ; au loin elle ressortirait vert vif. Une quantité de brume par touffe, d'après la
// profondeur de son centre et la brume du niveau, passe par TFACTOR (0x537360 appelé en 0x1A993D) et l'étage de
// texture 1 la mélange au dessin (DrawVertices en 0x1A9A37).
// Personnages : 0x80DA0 cache un personnage plus loin que son ushort +0xCA (1000) ; 0 = sans limite, ce que la
// fonction fait elle-même en cinématique. Fourrure : la distance mesurée par 0x3E1EE0 pour le nombre de coques
// (retour 0x8210C) est bornée à 0,4 R (R dans xmm3) : fourrure complète à toute distance.
#include "cdetv.h"

namespace game {
bool g_distance_loin = true;
}  // namespace game

namespace {
using game::cdetv::Ok;
using game::cdetv::RF;

uint32_t g_touffe = 0;     // la touffe dont le centre est posé sur la caméra
uint32_t g_centre[3];      // son vrai centre
uint32_t g_brume = 0;      // TFACTOR de la touffe en cours, 0 = sans brume
bool g_brume_dessin = false;

void RemettreCentre() {
    if (!g_touffe) return;
    for (int i = 0; i < 3; i++) kernel::W32(g_touffe + 0x2C + 4 * i, g_centre[i]);
    g_touffe = 0;
}

uint32_t Octet(uint32_t va) {
    int32_t c = int32_t(kernel::R32(va));
    return c < 0 ? 0u : c > 255 ? 255u : uint32_t(c);
}

// la brume du niveau au centre de la touffe (matrice de vue du joueur), avant que le centre ne bouge
void MesurerBrume(uint32_t touffe, int joueur) {
    uint32_t rec = kernel::R32(0x84A13C);
    if (!Ok(rec)) return;
    float loin = RF(0x771CE4), pres = RF(0x771CE8);
    if (loin == 0.0f || !(loin > pres)) return;  // pas de brume dans ce niveau
    uint32_t v = rec + uint32_t(joueur) * 0x2B0 + 0x40;
    if (!Ok(v, 0x40)) return;
    float prof = -(RF(touffe + 0x2C) * RF(v + 0x08) + RF(touffe + 0x30) * RF(v + 0x18) +
                   RF(touffe + 0x34) * RF(v + 0x28) + RF(v + 0x38));
    float a = (prof - pres) / (loin - pres);
    float max = RF(0x750300);
    if (a > max) a = max;
    if (a > 1.0f) a = 1.0f;
    uint32_t a8 = a > 0.0f ? uint32_t(a * 255.0f + 0.5f) : 0u;
    if (a8) g_brume = a8 << 24 | Octet(0x771CE0) << 16 | Octet(0x771CDC) << 8 | Octet(0x771CD8);
}
}  // namespace

// entrée de 0x1A9540 (dessin d'une touffe d'herbe) : [esp+4] touffe, [esp+8] short joueur
void DistanceHerbe(X86Context& ctx, uint8_t* base) {
    (void)base;
    RemettreCentre();  // jamais laissé déplacé par un appel manqué
    g_brume = 0;
    g_brume_dessin = false;
    uint32_t touffe = kernel::R32(ctx.esp + 4), joueurs = kernel::R32(0x8470DC);
    int16_t joueur = int16_t(kernel::R16(ctx.esp + 8));
    if (!game::g_distance_loin || !Ok(touffe, 0x38) || !Ok(joueurs) || joueur < 0 || joueur > 3) return;
    uint32_t cam = kernel::R32(joueurs + uint32_t(joueur) * 0x770 + 0x3E0);
    if (!Ok(cam, 0x84)) return;
    MesurerBrume(touffe, joueur);
    g_touffe = touffe;
    for (int i = 0; i < 3; i++) {
        g_centre[i] = kernel::R32(touffe + 0x2C + 4 * i);
        kernel::W32(touffe + 0x2C + 4 * i, kernel::R32(cam + 0x78 + 4 * i));
    }
}

// avant l'appel de la translation D3DX en 0x1A969F : x, y, z poussés en [esp], [esp+4], [esp+8]
void DistanceHerbeCentre(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_touffe) return;
    for (int i = 0; i < 3; i++) kernel::W32(ctx.esp + 4 * i, g_centre[i]);
    RemettreCentre();
}

// avant l'appel de SetVertexShaderConstant en 0x1A952C (ecx = registre 0x65, edx = 4 flottants) : sans poussée
void DistanceHerbePoussee(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!game::g_distance_loin || ctx.ecx != 0x65 || !Ok(ctx.edx, 16)) return;
    for (int i = 0; i < 4; i++) kernel::W32(ctx.edx + 4 * i, 0);
}

// avant l'appel de SetRenderState_TextureFactor en 0x1A993D (valeur en [esp]) : la brume de la touffe
void DistanceHerbeBrume(X86Context& ctx, uint8_t* base) {
    (void)base;
    if (!g_brume) return;
    kernel::W32(ctx.esp, g_brume);
    g_brume_dessin = true;
}

// avant l'appel de DrawVertices en 0x1A9A37 : étage 1 = BLENDFACTORALPHA (TFACTOR, CURRENT), alpha CURRENT, écrit
// dans l'état différé de D3D et dans le cache du jeu (0x7717xx), que son « posé si changé » remet ensuite
void DistanceHerbeDessin(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    if (!g_brume_dessin) return;
    g_brume_dessin = false;
    constexpr uint32_t kSelectArg1 = 2, kBlendFactorAlpha = 15, kCurrent = 1, kTFactor = 3;
    const uint32_t etats[][3] = {{0x5473E0, 0x7717E8, kBlendFactorAlpha},
                                 {0x5473E8, 0x7717C8, kTFactor},
                                 {0x5473EC, 0x7717B8, kCurrent},
                                 {0x5473F0, 0x7717A8, kSelectArg1},
                                 {0x5473F8, 0x771788, kCurrent}};
    for (auto& e : etats) {
        kernel::W32(e[0], e[2]);
        kernel::W32(e[1], e[2]);
    }
    kernel::W32(0x5499F0, kernel::R32(0x5499F0) | 0x800);  // états d'étages de texture modifiés
}

// 0x80DA0 (cdecl : personnage, masque de vue) : sans limite de distance pendant l'appel
X86_EXTERN_FUNC(__imp__sub_00080DA0);
X86_FUNC(sub_00080DA0) {
    uint32_t c = kernel::R32(ctx.esp + 4);
    if (!game::g_distance_loin || !Ok(c + 0xCA, 2)) {
        __imp__sub_00080DA0(ctx, base);
        return;
    }
    uint16_t limite = kernel::R16(c + 0xCA);
    kernel::W16(c + 0xCA, 0);
    __imp__sub_00080DA0(ctx, base);
    kernel::W16(c + 0xCA, limite);
}

// 0x3E1EE0 (cdecl : deux points, distance dans xmm0) : depuis la passe de fourrure (retour 0x8210C), bornée à 0,4 R
X86_EXTERN_FUNC(__imp__sub_003E1EE0);
X86_FUNC(sub_003E1EE0) {
    bool fourrure = kernel::R32(ctx.esp) == 0x8210C;
    __imp__sub_003E1EE0(ctx, base);
    if (!fourrure || !game::g_distance_loin) return;
    float complet = ctx.xmm[3].f[0] > 0.0f ? ctx.xmm[3].f[0] * 0.4f : 400.0f;
    if (ctx.xmm[0].f[0] > complet) ctx.xmm[0].f[0] = complet;
}
