// Instantanés : enregistrer la partie n'importe où et revenir plus tard à ce moment, sur le modèle du portage
// Wind Waker. Menu sur Back (manette 1, Retour arrière au clavier) ou F2 : haut/bas choisit l'emplacement,
// A reprend, X enregistre, B ou Back ferme ; pendant ce temps le jeu reçoit une manette au repos.
//
// Un instantané est le bloc de partie que le jeu met dans sa sauvegarde (0x353350 le remplit depuis l'état en cours :
// niveau, point de départ, vies, énergie, argent, drapeaux de tâches et d'histoire), plus la position de Conker.
// Le reprendre suit le chemin du menu de chargement du jeu (0x1FB627) : 0x353400 remet le bloc en place, 0x1FB910
// demande le niveau au point de départ ([0x7819D6] = 2) ; une fois le niveau chargé et la partie repartie
// ([0x7819D6] revenu à 0), Conker est replacé où il était. Les ennemis et objets du niveau repartent de zéro.
// Fichiers « Instantanés\emplacementN.cli » à côté de l'exécutable, à part des sauvegardes du jeu.
#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "xdk/hle.h"

namespace {

constexpr int kEmplacements = 6;

// le jeu
constexpr uint32_t kEtat = 0x7819D6;        // demande de changement de niveau, 0 en partie
constexpr uint32_t kMulti = 0x7819D5;       // partie multijoueur
constexpr uint32_t kNiveau = 0x7FA1F4;      // niveau en cours
constexpr uint32_t kNoms = 0x69CF90;        // noms des niveaux
constexpr uint32_t kJoueurs = 0x84A5F8;     // joueurs, 0x6D0 octets chacun ; position en +0x78
constexpr uint32_t kBloque1 = 0x76EC82, kBloque2 = 0x771130;  // 0x1FB910 ne demande rien quand l'un est mis
constexpr uint32_t kFrontend = 29, kPremierMulti = 0x4B;
constexpr uint32_t kCapturer = 0x353350;    // (objet de partie) : état en cours -> bloc
constexpr uint32_t kRemettre = 0x353400;    // ebx = objet de partie : bloc -> état en cours
constexpr uint32_t kContinuer = 0x1FB910;   // charge [0x84B248] au point [0x84B250]
// le bloc : octets 0x10 à 0x2B8 de l'objet de partie, tels que le jeu les copie (0x3531E0)
constexpr uint32_t kDebut = 0x10, kTaille = 0x2A8;
constexpr uint32_t kAttente = 3;            // images de jeu après le chargement avant de replacer Conker

struct Entete {
    char magic[4] = {'C', 'L', 'R', '1'};
    uint32_t version = 1;
    int64_t quand = 0;
    float pos[3] = {};
    char lieu[32] = {};
    uint32_t taille = kTaille;
};

struct Emplacement {
    bool pris = false;
    Entete e;
    uint8_t bloc[kTaille] = {};
    uint8_t Octet(uint32_t champ) const { return bloc[champ - kDebut]; }
    uint32_t Mot(uint32_t champ) const {
        uint32_t v;
        std::memcpy(&v, bloc + champ - kDebut, 4);
        return v;
    }
};

std::mutex g_verrou;  // emplacements et message
Emplacement g_emplacements[kEmplacements];
std::string g_message;
double g_message_fin = 0;  // -1 : pas encore affiché
bool g_charge = false;

std::atomic<bool> g_ouvert{false};
std::atomic<int> g_choix{0};
std::atomic<int> g_demande{0};  // 1 enregistrer, 2 reprendre g_choix
int g_demande_emplacement = 0;

// reprise en cours : Conker à replacer une fois le niveau chargé
struct Reprise {
    bool active = false, vu = false;
    uint32_t niveau = 0, images = 0;
    float pos[3] = {};
} g_reprise;

uint8_t Lire8(uint32_t a) { return kernel::G<uint8_t>(a); }

std::filesystem::path Dossier() { return app::ExeDir() / L"Instantanés"; }
std::filesystem::path Fichier(int i) { return Dossier() / ("emplacement" + std::to_string(i + 1) + ".cli"); }

void Message(std::string m) {
    LOGI("instantanés : %s", m.c_str());
    std::lock_guard lock(g_verrou);
    g_message = std::move(m);
    g_message_fin = -1;  // affiché 4 s à partir de la prochaine image
}

// une fois, verrou tenu
void Charger() {
    if (g_charge) return;
    g_charge = true;
    for (int i = 0; i < kEmplacements; i++) {
        std::ifstream f(Fichier(i), std::ios::binary);
        if (!f) continue;
        Emplacement e;
        f.read(reinterpret_cast<char*>(&e.e), sizeof(e.e));
        if (!f || std::memcmp(e.e.magic, "CLR1", 4) || e.e.version != 1 || e.e.taille != kTaille) {
            LOGW("instantanés : %s illisible", Fichier(i).string().c_str());
            continue;
        }
        f.read(reinterpret_cast<char*>(e.bloc), kTaille);
        if (!f) continue;
        e.e.lieu[sizeof(e.e.lieu) - 1] = 0;
        e.pris = true;
        g_emplacements[i] = e;
    }
}

bool Ecrire(int i, const Emplacement& e) {
    std::error_code ec;
    std::filesystem::create_directories(Dossier(), ec);
    auto tmp = Fichier(i);
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(&e.e), sizeof(e.e));
        f.write(reinterpret_cast<const char*>(e.bloc), kTaille);
        if (!f) return false;
    }
    std::filesystem::rename(tmp, Fichier(i), ec);
    return !ec;
}

// en partie solo, aucun changement de niveau en cours ; sinon pourquoi
const char* PasEnJeu() {
    uint32_t niveau = kernel::R32(kNiveau);
    if (Lire8(kMulti) || niveau >= kPremierMulti) return "pas en multijoueur";
    if (niveau == kFrontend) return "lancez d'abord une partie";
    if (Lire8(kEtat) || g_reprise.active) return "changement de niveau en cours";
    if (!kernel::R32(kJoueurs)) return "Conker n'est pas là";
    return nullptr;
}

// un objet de partie de travail sur la pile invitée, sous ce que l'appelant utilise
struct Travail {
    X86Context& ctx;
    uint32_t esp, obj;
    explicit Travail(X86Context& c) : ctx(c), esp(c.esp) {
        ctx.esp = (esp - 0x400) & ~15u;
        obj = ctx.esp + 0x80;
        std::memset(kernel::HostPtr(obj), 0xFF, 0x2C0);  // rien d'égal à l'état en cours : tout est recalculé
    }
    ~Travail() { ctx.esp = esp; }
};

void Enregistrer(X86Context& ctx, int i) {
    if (const char* r = PasEnJeu()) return Message(std::string("enregistrement impossible : ") + r);
    Emplacement e;
    {
        Travail t(ctx);
        kernel::CallGuest(ctx, kCapturer, {t.obj});
        std::memcpy(e.bloc, kernel::HostPtr(t.obj + kDebut), kTaille);
    }
    std::memcpy(e.e.pos, kernel::HostPtr(kernel::R32(kJoueurs) + 0x78), sizeof(e.e.pos));
    e.e.quand = int64_t(std::time(nullptr));
    uint32_t niveau = e.Octet(0x1A);
    uint32_t nom = niveau < 96 ? kernel::R32(kNoms + niveau * 4) : 0;
    std::snprintf(e.e.lieu, sizeof(e.e.lieu), "%s", nom ? kernel::ReadCString(nom, 31).c_str() : "?");
    e.pris = true;
    bool ok = Ecrire(i, e);
    {
        std::lock_guard lock(g_verrou);
        g_emplacements[i] = e;
    }
    char m[160];
    std::snprintf(m, sizeof(m), "emplacement %d : %s, point %u, position %.0f %.0f %.0f%s", i + 1, e.e.lieu,
                  e.Octet(0x1B), e.e.pos[0], e.e.pos[1], e.e.pos[2], ok ? "" : " (fichier non écrit)");
    Message(ok ? std::string("enregistré, ") + m : m);
}

void Reprendre(X86Context& ctx, int i) {
    Emplacement e;
    {
        std::lock_guard lock(g_verrou);
        e = g_emplacements[i];
    }
    if (!e.pris) return Message("emplacement " + std::to_string(i + 1) + " vide");
    if (const char* r = PasEnJeu()) return Message(std::string("reprise impossible : ") + r);
    if (Lire8(kBloque1) || Lire8(kBloque2)) return Message("reprise impossible pour l'instant");
    {
        Travail t(ctx);
        std::memcpy(kernel::HostPtr(t.obj + kDebut), e.bloc, kTaille);
        uint32_t ebx = ctx.ebx;
        ctx.ebx = t.obj;
        kernel::CallGuest(ctx, kRemettre, {});
        ctx.ebx = ebx;
        kernel::CallGuest(ctx, kContinuer, {});
    }
    if (Lire8(kEtat) != 2) {
        LOGW("instantanés : le jeu n'a pas demandé le niveau (état %u)", Lire8(kEtat));
        return Message("reprise refusée par le jeu");
    }
    g_reprise = {};
    g_reprise.active = true;
    g_reprise.niveau = e.Octet(0x1A);
    std::memcpy(g_reprise.pos, e.e.pos, sizeof(g_reprise.pos));
    Message("reprise de l'emplacement " + std::to_string(i + 1) + " (" + e.e.lieu + ")");
}

// chaque image : Conker replacé quelques images après le chargement
void SuivreReprise() {
    Reprise& r = g_reprise;
    if (!r.active) return;
    if (Lire8(kEtat)) {
        r.vu = true;
        r.images = 0;
        return;
    }
    if (!r.vu) {
        if (++r.images > 120) r.active = false;  // la demande n'a pas abouti
        return;
    }
    uint32_t joueur = kernel::R32(kJoueurs);
    if (kernel::R32(kNiveau) != r.niveau || !joueur) {
        LOGW("instantanés : niveau %u chargé au lieu de %u, Conker laissé au point de départ", kernel::R32(kNiveau), r.niveau);
        r.active = false;
        return;
    }
    if (++r.images < kAttente) return;
    if (r.images == kAttente) {
        float avant[3];
        std::memcpy(avant, kernel::HostPtr(joueur + 0x78), sizeof(avant));
        std::memcpy(kernel::HostPtr(joueur + 0x78), r.pos, sizeof(r.pos));
        LOGI("instantanés : Conker replacé de %.0f %.0f %.0f en %.0f %.0f %.0f", avant[0], avant[1], avant[2], r.pos[0],
             r.pos[1], r.pos[2]);
        return;
    }
    if (r.images < kAttente + 60) return;
    float apres[3];
    std::memcpy(apres, kernel::HostPtr(joueur + 0x78), sizeof(apres));
    LOGI("instantanés : 1 s plus tard, Conker en %.0f %.0f %.0f", apres[0], apres[1], apres[2]);
    r.active = false;
}

}  // namespace

// lecture des manettes (0x3D1B16, une fois par image, fil du jeu) : demandes du menu, puis la reprise en cours
void Instantanes(X86Context& ctx, uint8_t* base) {
    (void)base;
    {
        std::lock_guard lock(g_verrou);
        Charger();
    }
    if (int d = g_demande.exchange(0)) {
        if (d == 1) Enregistrer(ctx, g_demande_emplacement);
        else Reprendre(ctx, g_demande_emplacement);
    }
    // essais : XBE_INSTANTANE=<n> reprend l'emplacement n une fois la première partie en cours depuis 2 s
    static int essai = [] {
        const char* v = std::getenv("XBE_INSTANTANE");
        return v ? std::atoi(v) : 0;
    }();
    static int essai_images = 0;
    if (essai > 0 && essai <= kEmplacements && !PasEnJeu() && ++essai_images == 120) {
        Reprendre(ctx, essai - 1);
        essai = 0;
    }
    SuivreReprise();
}

namespace game {

bool InstantanesOuvert() { return g_ouvert; }

bool InstantanesTouche(uint32_t vk) {
    constexpr uint32_t kF2 = 0x71;  // VK_F2
    if (vk != kF2) return false;
    g_ouvert = !g_ouvert;
    return true;
}

// la manette 1 mène le menu ; ouvert, le jeu reçoit des manettes au repos
void InstantanesPad(int port, input::Pad& pad) {
    constexpr uint16_t kHaut = 0x01, kBas = 0x02, kBack = 0x20;
    if (port == 0) {
        static uint16_t avant = 0;
        uint16_t b = pad.buttons;
        if (pad.ly > 16000) b |= 0x100;  // stick gauche, comme la croix
        if (pad.ly < -16000) b |= 0x200;
        if (pad.analog[0] > 0x40) b |= 0x1000;  // A
        if (pad.analog[1] > 0x40) b |= 0x2000;  // B
        if (pad.analog[2] > 0x40) b |= 0x4000;  // X
        uint16_t neuf = b & ~avant;
        avant = b;
        bool solo = !Lire8(kMulti);
        if ((neuf & kBack) && (solo || g_ouvert)) g_ouvert = !g_ouvert;
        else if (g_ouvert) {
            if (neuf & (kHaut | 0x100)) g_choix = (g_choix + kEmplacements - 1) % kEmplacements;
            if (neuf & (kBas | 0x200)) g_choix = (g_choix + 1) % kEmplacements;
            if ((neuf & (0x1000 | 0x4000)) && !g_demande) {
                g_demande_emplacement = g_choix;
                g_demande = neuf & 0x4000 ? 1 : 2;
                if (neuf & 0x1000) g_ouvert = false;
            }
            if (neuf & 0x2000) g_ouvert = false;
        }
        if (solo) pad.buttons &= ~kBack;  // Back est au menu
    }
    if (g_ouvert) {
        bool branchee = pad.connected;
        pad = {};
        pad.connected = branchee;
    }
}

void InstantanesUi() {
    std::lock_guard lock(g_verrou);
    if (g_message_fin < 0) g_message_fin = ImGui::GetTime() + 4;
    bool message = !g_message.empty() && ImGui::GetTime() < g_message_fin;
    if (!g_ouvert && !message) return;
    Charger();
    ImGuiIO& io = ImGui::GetIO();
    if (!g_ouvert) {
        // le résultat seul, en bas de l'écran
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 48), ImGuiCond_Always, ImVec2(0.5f, 1));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##instantanés", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::SetWindowFontScale(1.6f);
        ImGui::TextUnformatted(g_message.c_str());
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool ouvert = true;
    if (ImGui::Begin("Instantanés (Back, F2)", &ouvert, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetWindowFontScale(1.6f);
        for (int i = 0; i < kEmplacements; i++) {
            const Emplacement& e = g_emplacements[i];
            char ligne[200];
            if (e.pris) {
                std::time_t t = std::time_t(e.e.quand);
                std::tm tm{};
                localtime_s(&tm, &t);
                char quand[32];
                std::strftime(quand, sizeof(quand), "%d/%m %H:%M", &tm);
                std::snprintf(ligne, sizeof(ligne), "%d   %s, point %u   %s   vies %u, argent %u", i + 1, e.e.lieu,
                              e.Octet(0x1B), quand, e.Octet(0x18), e.Mot(0x14));
            } else {
                std::snprintf(ligne, sizeof(ligne), "%d   (vide)", i + 1);
            }
            ImGui::PushID(i);
            if (ImGui::Selectable(ligne, g_choix == i)) g_choix = i;
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextDisabled("A : reprendre    X : enregistrer    B ou Back : fermer");
        if (message) ImGui::TextUnformatted(g_message.c_str());
    }
    ImGui::End();
    if (!ouvert) g_ouvert = false;
}

}  // namespace game
