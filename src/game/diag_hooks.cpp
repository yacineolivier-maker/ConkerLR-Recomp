// Hooks de diagnostic (config [[hook]]) : journalisent l'état de sous-systèmes du jeu en cours de mise au point.
#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include "app/app.h"
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

// test (0x2A9442, avant le chargement du niveau demandé [0x7FA1F4], point d'entrée [0x7FA1E4]) :
// XBE_NIVEAU=<nom de la table 0x69CF90 ou numéro> charge ce niveau à la place du premier niveau chargé après le menu,
// XBE_ENTREE=<n> son point d'entrée. Pour passer en revue le rendu des niveaux sans y jouer.
void ForceLevel(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    static bool done = false;
    static const char* want = std::getenv("XBE_NIVEAU");
    uint32_t level = kernel::R32(0x7FA1F4);
    LOGI("niveau : chargement du niveau %u (%s), entrée %u/%u", level,
         level < 96 && kernel::R32(0x69CF90 + level * 4) ? kernel::ReadCString(kernel::R32(0x69CF90 + level * 4)).c_str() : "?",
         kernel::R32(0x7FA1E4), kernel::R32(0x7FA1E8));
    if (!want || done || level == 29) return;  // le premier niveau après le menu (Frontend)
    done = true;
    uint32_t id = ~0u;
    if (*want >= '0' && *want <= '9') id = uint32_t(std::atoi(want));
    for (uint32_t i = 0; i < 96 && id == ~0u; i++)
        if (uint32_t n = kernel::R32(0x69CF90 + i * 4); n && _stricmp(kernel::ReadCString(n).c_str(), want) == 0) id = i;
    if (id == ~0u) {
        LOGW("niveau : %s absent de la table", want);
        return;
    }
    kernel::W32(0x7FA1F4, id);
    if (const char* e = std::getenv("XBE_ENTREE")) kernel::W32(0x7FA1E4, uint32_t(std::atoi(e)));
    LOGI("niveau : test, niveau %u (%s) chargé à la place", id, want);
}

// essais (XBE_AUTOPAD ou XBE_ARRIERE_PLAN) : « Niveau N : nom » du niveau en cours ([0x7FA1F8]) en haut de l'écran,
// pour reconnaître les niveaux d'une tournée à l'œil
namespace game {
void EtiquetteNiveau() {
    static const bool essai = std::getenv("XBE_AUTOPAD") || app::Background();
    if (!essai) return;
    uint32_t level = kernel::R32(0x7FA1F8);
    uint32_t nom = level < 96 ? kernel::R32(0x69CF90 + level * 4) : 0;
    char texte[96];
    std::snprintf(texte, sizeof(texte), "Niveau %u : %s", level, nom ? kernel::ReadCString(nom).c_str() : "?");
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 8), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##niveau", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(1.8f);
    ImGui::TextUnformatted(texte);
    ImGui::End();
}
}  // namespace game

// entrée de la trace de débogage du jeu (0x18B30, printf vers OutputDebugString, muette sans [0x84725D]) :
// XBE_TRACE_JEU=<octets drapeaux en hexadécimal, ex. 846FF3,846FFB> allume la trace et ces drapeaux de sous-systèmes
// (0x846FEF-0x846FFF : audio...), chaque message journalisé « jeu : ... »
void GameTrace(X86Context& ctx, uint8_t* base) {
    (void)base;
    static const char* flags = std::getenv("XBE_TRACE_JEU");
    if (!flags) return;
    uint32_t ap = ctx.esp + 8;
    std::string fmt = kernel::ReadCString(kernel::R32(ctx.esp + 4), 512), out;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && std::strchr("-+ #0123456789.lhI", fmt[i])) spec += fmt[i++];
        if (i >= fmt.size()) break;
        char c = fmt[i], buf[300];
        if (c == '%') {
            out += '%';
        } else if (c == 's') {
            out += kernel::ReadCString(kernel::R32(ap), 256);
            ap += 4;
        } else if (c == 'f' || c == 'g' || c == 'e') {
            double d;
            std::memcpy(&d, kernel::HostPtr(ap), 8);
            ap += 8;
            std::snprintf(buf, sizeof(buf), (spec + c).c_str(), d);
            out += buf;
        } else {
            std::string clean;
            for (char ch : spec)
                if (ch != 'l' && ch != 'h' && ch != 'I') clean += ch;
            std::snprintf(buf, sizeof(buf), (clean + (c == 'p' ? 'x' : c)).c_str(), kernel::R32(ap));
            ap += 4;
            out += buf;
        }
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    LOGI("jeu : %s", out.c_str());
}
