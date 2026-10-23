// Panneau d'options graphiques en jeu (F1) : les réglages du rendu modernisé et de l'affichage, appliqués aussitôt,
// enregistrés dans settings.toml par le bouton du panneau. Souris et clavier ; la manette reste au jeu.
#include <imgui.h>

#include <cstdint>
#include <iterator>

#include "app/app.h"

namespace game {

// rendu.cpp
extern bool g_smaa, g_occlusion, g_ombres_douces;
extern uint32_t g_occlusion_rayon, g_occlusion_force, g_nettete, g_ombres_taille;

namespace {

bool g_open = false;
const char* g_saved = nullptr;  // message après l'enregistrement
double g_saved_until = 0;

// une valeur entière bornée, réglée par une glissière
void Slider(const char* label, uint32_t& v, int lo, int hi, const char* fmt) {
    int x = int(v);
    if (ImGui::SliderInt(label, &x, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) v = uint32_t(x);
}

// un choix parmi des valeurs fixes
void Choice(const char* label, uint32_t& v, const uint32_t* values, const char* const* names, int n) {
    int at = 0;
    for (int i = 0; i < n; i++)
        if (values[i] == v) at = i;
    if (ImGui::Combo(label, &at, names, n)) v = values[at];
}

}  // namespace

bool OptionsKey(uint32_t vk) {
    constexpr uint32_t kF1 = 0x70;  // VK_F1
    if (vk != kF1) return false;
    g_open = !g_open;
    return true;
}

void OptionsUi(double fps) {
    if (!g_open) return;
    app::Settings& s = app::Get();
    ImGui::SetNextWindowPos(ImVec2(48, 48), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Options graphiques (F1)", &g_open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("%.0f images par seconde", fps);
    ImGui::Separator();

    ImGui::TextUnformatted("Ombres et lumière");
    ImGui::Checkbox("Ombres douces", &g_ombres_douces);
    // prise à la création des textures d'ombre : au prochain lancement
    static const uint32_t kShadowSizes[] = {0, 512, 1024, 2048};
    static const char* const kShadowSizeNames[] = {"Celle du jeu", "512", "1024", "2048"};
    Choice("Taille des ombres (relancer)", g_ombres_taille, kShadowSizes, kShadowSizeNames, int(std::size(kShadowSizes)));
    ImGui::Checkbox("Occlusion ambiante (GTAO)", &g_occlusion);
    if (g_occlusion) {
        Slider("Portée", g_occlusion_rayon, 5, 200, "%d");
        Slider("Intensité", g_occlusion_force, 0, 200, "%d %%");
    }
    ImGui::Separator();

    ImGui::TextUnformatted("Image");
    ImGui::Checkbox("Anticrénelage SMAA", &g_smaa);
    Slider("Netteté", g_nettete, 0, 100, "%d %%");
    static const uint32_t kAniso[] = {0, 2, 4, 8, 16};
    static const char* const kAnisoNames[] = {"Au choix du jeu", "2x", "4x", "8x", "16x"};
    Choice("Filtrage anisotrope", s.aniso, kAniso, kAnisoNames, int(std::size(kAniso)));
    Slider("Résolution du rendu", s.render_percent, 50, 200, "%d %%");
    ImGui::Separator();

    ImGui::TextUnformatted("Affichage");
    ImGui::Checkbox("Synchronisation verticale", &s.vsync);
    static const uint32_t kCaps[] = {30, 60, 120, 144, app::kFpsCapRefresh, 0};
    static const char* const kCapNames[] = {"30", "60", "120", "144", "Écran", "Sans limite"};  // Écran : juste sous sa fréquence
    Choice("Images par seconde au plus", s.fps_cap, kCaps, kCapNames, int(std::size(kCaps)));
    ImGui::Separator();

    if (ImGui::Button("Enregistrer")) {
        app::Save();
        g_saved = "Réglages enregistrés dans settings.toml";
        g_saved_until = ImGui::GetTime() + 3;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fermer")) g_open = false;
    if (g_saved && ImGui::GetTime() < g_saved_until) ImGui::TextUnformatted(g_saved);
    ImGui::TextDisabled("F7 SMAA, F8 occlusion, F9 ombres douces");
    ImGui::End();
}

}  // namespace game
