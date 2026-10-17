#include "app/app.h"

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
    .options = {},
    .load = nullptr,
    .save = nullptr,
    .init = nullptr,
    .loaded = nullptr,
    .window = nullptr,
    .message = nullptr,
    .ui = nullptr,
    .pad = nullptr,
    .rumble = nullptr,
};
