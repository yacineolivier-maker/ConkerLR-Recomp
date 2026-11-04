// Cadence : 60 images/s et vitesse juste en cas de ralentissement.
// Repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (THIRD_PARTY.md) ; adresses et principe de ce projet, code réécrit ici.
//
// 0x2A7B60 (une fois par image) fixe le pas du jeu : vblanks [0x7FA20C] = intervalle de présentation [0x6C0268]
// + vblanks manqués par le dernier Swap [0x7F9F30], plafonné à 60 ; pas [0x7FA24C] = vblanks / 2. Le jeu est donc
// prévu pour d'autres intervalles : à l'intervalle 1 le pas vaut 0,5 et la vitesse reste juste.
// - 60 images/s : là où le jeu demande l'intervalle 2 à la réinitialisation vidéo (0x2A78D0, edx), il reçoit 1.
//   Le réglage pris en cours de partie s'applique au début de l'image suivante (crochet de 0x2A7B60).
// - Vitesse juste : sur Xbox, le rappel de Swap du jeu (0x2A8020) écrit les vblanks manqués ; notre runtime ne
//   l'appelle pas, [0x7F9F30] reste à 0 et une image en retard ralentit le jeu. On les compte ici, d'après le temps
//   écoulé entre deux images (hors pause de la fenêtre). cdetv retire au contraire un vblank compté en trop par son
//   runtime : même but, le pas suit le temps réel.
#include "app/app.h"
#include "cdetv.h"
#include "core/log.h"
#include "xdk/d3d8.h"

namespace game {
bool g_cadence_60 = false;
bool g_vitesse_juste = true;
}  // namespace game

namespace {
uint32_t g_demande = 0;  // l'intervalle demandé par le jeu à sa dernière réinitialisation vidéo
int64_t g_avant = 0;     // l'horloge du jeu à l'image précédente
double g_retard = 0;     // vblanks de retard reportés d'une image à l'autre

uint32_t Voulu(uint32_t demande) { return demande == 2 && game::g_cadence_60 ? 1 : demande; }
}  // namespace

// entrée de 0x2A78D0 (réinitialisation vidéo) : edx = intervalle, al = drapeau IMMEDIATE
void CadenceReset(X86Context& ctx, uint8_t* base) {
    (void)base;
    g_demande = ctx.edx & 0xFF;
    uint32_t voulu = Voulu(g_demande);
    LOGI("cadence: réinitialisation vidéo, intervalle %u%s (appel de %08X)", g_demande,
         voulu != g_demande ? " -> 1 (60 images/s)" : "", kernel::R32(ctx.esp));
    ctx.edx = (ctx.edx & ~0xFFu) | voulu;
}

// entrée de 0x2A7B60 (chronomètre d'image)
void CadenceImage(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    using game::cdetv::R8;
    using game::cdetv::W8;
    if (g_demande) {
        uint32_t voulu = Voulu(g_demande);
        if (R8(0x6C0268) != voulu) {
            W8(0x6C0268, uint8_t(voulu));
            d3d::SetPresentInterval(voulu);
            LOGI("cadence: intervalle %u (%s images/s)", voulu, voulu == 1 ? "60" : voulu == 2 ? "30" : "autre");
        }
    }
    int64_t now = app::GameNs();
    int64_t ecart = g_avant ? now - g_avant : 0;
    g_avant = now;
    if (!game::g_vitesse_juste || !ecart) {
        g_retard = 0;
        return;
    }
    uint32_t intervalle = R8(0x6C0268);
    if (!intervalle) intervalle = 1;
    constexpr double kVblank = 1e9 / 60;
    g_retard += double(ecart) / kVblank - intervalle;
    // la gigue d'une image à l'autre se compense ; un à-coup (jusqu'à une demi-seconde) est rattrapé 4 vblanks par
    // image sur les images suivantes ; un long arrêt (chargement) ne l'est pas. Essai du 2026-11-04 : avec la
    // remise à zéro au-delà de 4, 54 images/s ne donnaient que 8 vblanks manqués en 10 s (temps du jeu 0,91).
    if (g_retard < -1) g_retard = -1;
    if (g_retard > 30) g_retard = 0;
    uint32_t manques = 0;
    if (g_retard >= 1) {
        manques = g_retard >= 4 ? 4 : uint32_t(g_retard);
        g_retard -= manques;
    }
    kernel::W32(0x7F9F30, manques);
}
