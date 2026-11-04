// Outils d'essai : journal d'événements dans un fichier, actions minutées, raccourcis de débogage, téléportation.
// Sans variable XBE_ ni touche Ctrl, rien ne change pour le joueur (le journal n'est écrit qu'en essai).
//
// - Journal : logs\evenements.txt à côté de l'exe (refait à chaque lancement), une ligne par événement, l'instant en
//   secondes depuis le lancement : niveau chargé, pause du jeu, intervalle de présentation, réglage changé, position
//   de Conker, repère, téléportation ; toutes les 10 s la cadence mesurée (images/s, pas moyen, vblanks manqués,
//   temps du jeu par seconde réelle : 1,00 = vitesse juste). Écrit en essai (XBE_AUTOPAD, XBE_ACTIONS, arrière-plan
//   ou XBE_JOURNAL=1) ou dès qu'un raccourci sert.
// - XBE_ACTIONS="secondes:action;..." (instant depuis le lancement, comme les captures de lancer.ps1) :
//     cadence_60=0|1  vitesse_juste=0|1  distance_loin=0|1  sons_pause=0|1  berri_sol=0|1  flou_cinematiques=0|1
//     lueur=0..100  memoriser  teleporter  teleporter=x,y,z  position  repere=texte
//     niveau=<nom|numéro>[,entrée] (changement de niveau en cours de partie, comme le jeu)
//   Instant 0 : réglages pris dès la première image (sans toucher settings.toml).
// - Raccourcis (Ctrl + chiffre de la rangée du haut, fenêtre au premier plan) : 1 60 images/s, 2 vitesse juste,
//   3 distance, 4 sons en pause, 5 Berri, 6 flou des cinématiques, 7 lueur 100/50/0, 8 mémoriser la position de
//   Conker, 9 y revenir, 0 repère dans le journal.
// - Démarrage direct : XBE_NIVEAU=<nom|numéro> [XBE_ENTREE=<n>] (diag_hooks.cpp), état : XBE_INSTANTANE=<n>
//   (instantanes.cpp).
// Téléportation : la position de Conker (personnage d'identifiant 1, +0x10B, en service si +0x64 ; position
// +0x78/+0x7C/+0x80 ; table [0x84A5F8], 0x81 x 0x6D0, d'après cdetv/conker-live-recompiled, MIT) est réécrite ;
// la physique peut la corriger à l'image suivante (à vérifier en jeu).
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "cdetv.h"
#include "core/log.h"

extern "C" __declspec(dllimport) short __stdcall GetKeyState(int);  // user32 (sans windows.h ici)

namespace game {

namespace {
using cdetv::Ok;
using cdetv::RF;

const auto g_debut = std::chrono::steady_clock::now();
double Secondes() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_debut).count(); }

std::atomic<bool> g_journal_actif = false;
FILE* g_journal = nullptr;
std::mutex g_verrou_journal;
std::atomic<uint32_t> g_touche = 0;  // raccourci reçu par la fenêtre, traité à l'image suivante (fil du jeu)

void Journal(const char* fmt, ...) {
    if (!g_journal_actif) return;
    std::lock_guard lock(g_verrou_journal);
    if (!g_journal) {
        std::error_code ec;
        std::filesystem::create_directories(app::ExeDir() / "logs", ec);
        g_journal = _wfopen((app::ExeDir() / "logs" / "evenements.txt").c_str(), L"w");
        if (!g_journal) {
            g_journal_actif = false;
            return;
        }
        std::fprintf(g_journal, "# journal d'événements (secondes depuis le lancement)\n");
    }
    char ligne[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(ligne, sizeof(ligne), fmt, a);
    va_end(a);
    std::fprintf(g_journal, "%8.2f  %s\n", Secondes(), ligne);
    std::fflush(g_journal);
    LOGI("événement : %s", ligne);
}

std::string NomNiveau(uint32_t n) {
    uint32_t nom = n < 96 ? kernel::R32(0x69CF90 + n * 4) : 0;
    return nom ? kernel::ReadCString(nom, 64) : "?";
}

// Conker : le personnage d'identifiant 1 en service
uint32_t Conker() {
    uint32_t persos = kernel::R32(0x84A5F8);
    if (!Ok(persos, 0x81 * 0x6D0)) return 0;
    for (uint32_t i = 0; i < 0x81; i++) {
        uint32_t c = persos + i * 0x6D0;
        if (kernel::R32(c + 0x64) && cdetv::R8(c + 0x10B) == 1) return c;
    }
    return 0;
}

float g_memo[3];
bool g_memo_ok = false;

void Position(const char* quoi) {
    uint32_t c = Conker();
    if (!c) {
        Journal("%s : Conker absent", quoi);
        return;
    }
    Journal("%s : Conker en %.1f %.1f %.1f (niveau %u)", quoi, RF(c + 0x78), RF(c + 0x7C), RF(c + 0x80),
            kernel::R32(0x7FA1F8));
}

void Memoriser() {
    uint32_t c = Conker();
    if (!c) return Journal("mémoriser : Conker absent");
    for (int i = 0; i < 3; i++) g_memo[i] = RF(c + 0x78 + 4 * i);
    g_memo_ok = true;
    Position("mémoriser");
}

void Teleporter(const float* p) {
    uint32_t c = Conker();
    if (!c) return Journal("téléporter : Conker absent");
    if (!p) {
        if (!g_memo_ok) return Journal("téléporter : aucune position mémorisée");
        p = g_memo;
    }
    for (int i = 0; i < 3; i++) cdetv::WF(c + 0x78 + 4 * i, p[i]);
    Journal("téléporter : Conker posé en %.1f %.1f %.1f", p[0], p[1], p[2]);
}

uint32_t NumeroNiveau(const std::string& nom) {
    if (!nom.empty() && nom[0] >= '0' && nom[0] <= '9') return uint32_t(std::atoi(nom.c_str()));
    for (uint32_t i = 0; i < 96; i++)
        if (uint32_t n = kernel::R32(0x69CF90 + i * 4); n && _stricmp(kernel::ReadCString(n, 64).c_str(), nom.c_str()) == 0)
            return i;
    return ~0u;
}

// changer de niveau en cours de partie, comme le jeu le fait lui-même (0x9F030, cas 1 de 0x2A94A0) : entrée
// [0x7FA1E4], [0x7FA1E8] = 1, niveau [0x7FA1F4], puis l'état de la boucle principale [0x7819D6] = 2 (charger le
// niveau demandé), pris au tour suivant de la boucle (0x2A93F2). Refusé comme le jeu pendant un chargement.
void ChangerNiveau(const std::string& val) {
    size_t v = val.find(',');
    std::string nom = val.substr(0, v);
    uint32_t id = NumeroNiveau(nom), entree = v == std::string::npos ? 0 : uint32_t(std::atoi(val.c_str() + v + 1));
    if (id >= 96 || !kernel::R32(0x69CF90 + id * 4)) return Journal("niveau : « %s » absent de la table", nom.c_str());
    if (cdetv::R8(0x76EC82) || cdetv::R8(0x771130) || cdetv::R8(0x7819D6))
        return Journal("niveau : changement refusé (chargement en cours, état %u)", cdetv::R8(0x7819D6));
    kernel::W32(0x7FA1E8, 1);
    kernel::W32(0x7FA1E4, entree);
    kernel::W32(0x7FA1F4, id);
    kernel::W16(0x800400, 0);
    cdetv::W8(0x771B9C, 0);
    cdetv::W8(0x7819D6, 2);
    Journal("niveau : changement demandé vers %u (%s), entrée %u", id, NomNiveau(id).c_str(), entree);
}

// un réglage par son nom (ceux de settings.toml)
bool Regler(const std::string& nom, int v) {
    struct B {
        const char* nom;
        bool* b;
        uint32_t* u;
    };
    const B tous[] = {{"cadence_60", &g_cadence_60, nullptr},       {"vitesse_juste", &g_vitesse_juste, nullptr},
                      {"distance_loin", &g_distance_loin, nullptr}, {"sons_pause", &g_sons_pause, nullptr},
                      {"berri_sol", &g_berri_sol, nullptr},         {"flou_cinematiques", nullptr, &g_flou_cine},
                      {"lueur", nullptr, &g_lueur}};
    for (const B& r : tous) {
        if (nom != r.nom) continue;
        if (r.b) *r.b = v != 0;
        if (r.u) *r.u = uint32_t(v < 0 ? 0 : v);
        Journal("réglage %s = %d", r.nom, v);
        return true;
    }
    return false;
}

void Action(const std::string& a) {
    size_t eq = a.find('=');
    std::string nom = a.substr(0, eq), val = eq == std::string::npos ? "" : a.substr(eq + 1);
    if (nom == "memoriser") return Memoriser();
    if (nom == "position") return Position("position");
    if (nom == "repere") return Journal("repère : %s", val.c_str());
    if (nom == "niveau") return ChangerNiveau(val);
    if (nom == "teleporter") {
        if (val.empty()) return Teleporter(nullptr);
        float p[3] = {};
        if (std::sscanf(val.c_str(), "%f,%f,%f", &p[0], &p[1], &p[2]) == 3) return Teleporter(p);
        return Journal("téléporter : position illisible « %s »", val.c_str());
    }
    if (!val.empty() && Regler(nom, std::atoi(val.c_str()))) return;
    Journal("action inconnue « %s »", a.c_str());
}

struct Minutee {
    double at;
    std::string action;
    bool faite;
};

std::vector<Minutee>& Actions() {
    static std::vector<Minutee> out = [] {
        std::vector<Minutee> v;
        const char* e = std::getenv("XBE_ACTIONS");
        if (!e) return v;
        std::string s(e);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t fin = s.find(';', pos);
            std::string item = s.substr(pos, fin == std::string::npos ? std::string::npos : fin - pos);
            pos = fin == std::string::npos ? s.size() : fin + 1;
            size_t c = item.find(':');
            if (c != std::string::npos) v.push_back({std::atof(item.c_str()), item.substr(c + 1), false});
        }
        return v;
    }();
    return out;
}

// mesure de la cadence sur 10 s
struct Mesure {
    double debut = -1;
    uint32_t images = 0, manques = 0;
    double pas = 0;
} g_mesure;

void Raccourci(uint32_t vk) {
    switch (vk) {
    case '1': Regler("cadence_60", !g_cadence_60); break;
    case '2': Regler("vitesse_juste", !g_vitesse_juste); break;
    case '3': Regler("distance_loin", !g_distance_loin); break;
    case '4': Regler("sons_pause", !g_sons_pause); break;
    case '5': Regler("berri_sol", !g_berri_sol); break;
    case '6': Regler("flou_cinematiques", g_flou_cine ? 0 : 1); break;
    case '7': Regler("lueur", g_lueur >= 100 ? 50 : g_lueur >= 50 ? 0 : 100); break;
    case '8': Memoriser(); break;
    case '9': Teleporter(nullptr); break;
    case '0': Journal("repère (Ctrl+0)"); break;
    }
}

}  // namespace

// une fois par image (crochet OutilsImage, ci-dessous)
void ImageOutils() {
    static bool init = false;
    if (!init) {
        init = true;
        g_journal_actif = g_journal_actif || std::getenv("XBE_AUTOPAD") || std::getenv("XBE_ACTIONS") || std::getenv("XBE_JOURNAL") ||
                          app::Background();
        Journal("première image ; 60 images/s %d, vitesse juste %d, distance %d, sons en pause %d, Berri %d, "
                "flou %u, lueur %u", g_cadence_60, g_vitesse_juste, g_distance_loin, g_sons_pause, g_berri_sol,
                g_flou_cine, g_lueur);
    }
    double t = Secondes();
    for (Minutee& m : Actions())
        if (!m.faite && t >= m.at) {
            m.faite = true;
            Action(m.action);
        }
    if (uint32_t vk = g_touche.exchange(0)) Raccourci(vk);
    if (!g_journal_actif) return;

    static uint32_t niveau = ~0u, intervalle = ~0u;
    static int pause = -1;
    if (uint32_t n = kernel::R32(0x7FA1F8); n != niveau) {
        niveau = n;
        Journal("niveau %u : %s", n, NomNiveau(n).c_str());
    }
    if (int p = cdetv::R8(0x7FA275); p != pause) {
        pause = p;
        Journal(p ? "pause du jeu" : "fin de la pause du jeu");
    }
    if (uint32_t i = cdetv::R8(0x6C0268); i != intervalle) {
        intervalle = i;
        Journal("intervalle de présentation %u", i);
    }
    // l'image précédente : son pas et ses vblanks manqués
    if (g_mesure.debut < 0) g_mesure.debut = t;
    g_mesure.images++;
    g_mesure.pas += RF(0x7FA24C);
    g_mesure.manques += kernel::R32(0x7F9F30);
    if (t - g_mesure.debut >= 10) {
        double duree = t - g_mesure.debut;
        Journal("cadence : %.1f images/s, pas moyen %.3f, %u vblanks manqués, temps du jeu par seconde %.2f",
                g_mesure.images / duree, g_mesure.pas / g_mesure.images, g_mesure.manques,
                g_mesure.pas / 30.0 / duree);
        static uint32_t sons = 0, appels = 0, flou = 0, berri = 0;
        Journal("correctifs : %u échéances de sons gelées en pause (%u mises à jour d'émetteurs pendant la pause), "
                "%u flous de cinématique sautés, %u images de Berri à plat", g_compte_sons - sons,
                g_appels_sons_pause - appels, g_compte_flou - flou, g_compte_berri - berri);
        sons = g_compte_sons;
        appels = g_appels_sons_pause;
        flou = g_compte_flou;
        berri = g_compte_berri;
        Position("position");
        g_mesure = Mesure{};
        g_mesure.debut = t;
    }
}

// raccourcis : Ctrl + 1..0 (rangée du haut), fil de la fenêtre
bool OutilsTouche(uint32_t vk) {
    constexpr int kControl = 0x11;  // VK_CONTROL
    if (vk < '0' || vk > '9' || !(GetKeyState(kControl) & 0x8000)) return false;
    g_journal_actif = true;
    g_touche = vk;
    return true;
}

}  // namespace game

// entrée de 0x2A7B60 (une fois par image) ; le pas [0x7FA24C] et les vblanks manqués [0x7F9F30] lus ici sont ceux
// de l'image précédente, avant que CadenceImage et le jeu ne les refassent
void OutilsImage(X86Context& ctx, uint8_t* base) {
    (void)ctx;
    (void)base;
    game::ImageOutils();
}
