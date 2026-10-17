// Registres de l'APU MCPX (0xFE800000) : le moteur XACT lié au jeu et le code DirectSound d'origine les lisent
// directement. Le son passe par le DirectSound du runtime ; ici la page existe et le compteur global
// d'échantillons (NV_PAPU_XGSCNT, 0xFE80200C) avance à 48 kHz, ce qui suffit à leurs mesures de temps.
#include <chrono>
#include <thread>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

namespace {
constexpr uint32_t kApuBase = 0xFE800000, kApuSize = 0x80000;
constexpr uint32_t kGlobalSampleCount = kApuBase + 0x200C;
}  // namespace

void StartApu() {
    if (!mem::Commit(kApuBase, kApuSize)) {
        LOGE("apu: la page des registres n'a pas pu être réservée");
        return;
    }
    std::thread([] {
        auto start = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
            kernel::W32(kGlobalSampleCount, uint32_t(uint64_t(us.count()) * 48000 / 1000000));
        }
    }).detach();
    LOGI("apu: registres simulés à %08X", kApuBase);
}

}  // namespace game
