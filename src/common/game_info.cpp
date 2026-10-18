#include "app/app.h"
#include "core/input.h"

namespace game {
void StartHangWatch();  // hang_watch.cpp
void StartApu();        // apu.cpp
void AutoPad(int port, input::Pad& pad);  // autopad.cpp
// rendu.cpp
extern bool g_smaa, g_occlusion, g_ombres_douces;
extern uint32_t g_occlusion_rayon, g_occlusion_force, g_nettete;
void InstallRendu();
bool RenduKey(uint32_t vk);
// options.cpp
bool OptionsKey(uint32_t vk);
void OptionsUi(double fps);

void Init() {
    StartHangWatch();
    InstallRendu();
}

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
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
                {"smaa", &game::g_smaa, nullptr},
                {"occlusion_ambiante", &game::g_occlusion, nullptr},
                {"occlusion_rayon", nullptr, &game::g_occlusion_rayon},
                {"occlusion_force", nullptr, &game::g_occlusion_force},
                {"nettete", nullptr, &game::g_nettete}},
    .load = nullptr,
    .save = nullptr,
    .init = game::Init,
    .loaded = game::StartApu,
    .window = nullptr,
    .message = game::Message,
    .ui = game::OptionsUi,
    .pad = game::AutoPad,
    .rumble = nullptr,
};
