// Correctifs repris de Conker: Live & Recompiled (github.com/cdetv/conker-live-recompiled, src/recomp_manual.c),
// Copyright (c) 2026 cdetv, licence MIT (texte dans THIRD_PARTY.md). Les adresses et le principe de chaque correctif
// viennent de ce projet (même XBE PAL) ; le code est réécrit pour ce runtime, sans rien du code du jeu traduit.
#pragma once
#include <cstdint>
#include <cstring>

#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

namespace game {

// réglages (settings.toml, panneau F1 « Améliorations »)
extern bool g_cadence_60;      // cdetv_cadence.cpp : 60 images/s là où le jeu en demande 30
extern bool g_vitesse_juste;   // cdetv_cadence.cpp : les vblanks manqués comptés, le jeu garde sa vitesse
extern bool g_distance_loin;   // cdetv_distance.cpp : herbe, personnages et fourrure sans disparition au loin
extern bool g_sons_pause;      // cdetv_sons_pause.cpp : sons en cours gelés pendant la pause
extern bool g_berri_sol;       // cdetv_berri.cpp : Berri morte posée au sol, pas dedans
extern uint32_t g_flou_cine;   // cdetv_cinematiques.cpp : 0 sans flou de profondeur, 1 celui du jeu
extern uint32_t g_lueur;       // cdetv_lueur.cpp : intensité de la lueur en %, 100 = celle du jeu
// combien de fois chaque correctif a agi (relevés du journal d'essai, outils_test.cpp)
extern uint32_t g_compte_sons, g_appels_sons, g_appels_sons_pause, g_compte_flou, g_compte_berri;

namespace cdetv {

// une adresse invitée lisible (mémoire réservée et engagée)
inline bool Ok(uint32_t va, uint32_t size = 4) {
    return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1);
}

inline float RF(uint32_t va) {
    float f;
    uint32_t u = kernel::R32(va);
    std::memcpy(&f, &u, 4);
    return f;
}

inline void WF(uint32_t va, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    kernel::W32(va, u);
}

inline uint8_t R8(uint32_t va) { return *static_cast<uint8_t*>(kernel::HostPtr(va)); }
inline void W8(uint32_t va, uint8_t v) { *static_cast<uint8_t*>(kernel::HostPtr(va)) = v; }

}  // namespace cdetv
}  // namespace game
