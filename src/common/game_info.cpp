#include "app/app.h"
#include "core/input.h"

namespace game {
void StartHangWatch();  // hang_watch.cpp
void StartApu();        // apu.cpp
void AutoPad(int port, input::Pad& pad);  // autopad.cpp
// clavier_souris.cpp
void ClavierCharger(const void* table);
void ClavierEnregistrer(void* table);
void ClavierFenetre(void* hwnd);
bool ClavierMessage(uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void ClavierSouris(int port, input::Pad& pad);
// rendu.cpp
extern bool g_smaa, g_occlusion, g_ombres_douces;
extern uint32_t g_occlusion_rayon, g_occlusion_force, g_nettete, g_ombres_taille;
void InstallRendu();
bool RenduKey(uint32_t vk);
// options.cpp
bool OptionsKey(uint32_t vk);
void OptionsUi(double fps);

void Pad(int port, input::Pad& pad) {
    AutoPad(port, pad);
    ClavierSouris(port, pad);
}

void Init() {
    StartHangWatch();
    InstallRendu();
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (ClavierMessage(msg, wp, lp, result)) return true;
    constexpr uint32_t kKeyDown = 0x0100;  // WM_KEYDOWN
    if (msg != kKeyDown || !(RenduKey(uint32_t(wp)) || OptionsKey(uint32_t(wp)))) return false;
    *result = 0;
    return true;
}
}

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Conker: Live & Reloaded",
    .short_title = L"Conker",
    .name = L"ConkerLiveReloaded",
    .log = "conker",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Placez le jeu dans le dossier assets : son image disque (.iso), ou default.xbe avec ses "
               L"dossiers (dvddata, media) à côté.",
    .images = {{"default.xbe", "Conker"}},
    .options = {{"ombres_douces", &game::g_ombres_douces, nullptr},
                {"ombres_taille", nullptr, &game::g_ombres_taille},
                {"smaa", &game::g_smaa, nullptr},
                {"occlusion_ambiante", &game::g_occlusion, nullptr},
                {"occlusion_rayon", nullptr, &game::g_occlusion_rayon},
                {"occlusion_force", nullptr, &game::g_occlusion_force},
                {"nettete", nullptr, &game::g_nettete}},
    .load = game::ClavierCharger,
    .save = game::ClavierEnregistrer,
    .init = game::Init,
    .loaded = game::StartApu,
    .window = game::ClavierFenetre,
    .message = game::Message,
    .ui = game::OptionsUi,
    .pad = game::Pad,
    .rumble = nullptr,
};
