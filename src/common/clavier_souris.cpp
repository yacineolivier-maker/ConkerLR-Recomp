// Clavier et souris en dépannage, ajoutés à la manette 1 : déplacement par les touches (ZQSD en AZERTY, WASD en
// QWERTY : les positions physiques), caméra à la souris sur le stick droit, boutons sur des touches ou des clics.
// Touches réassignables dans settings.toml, table [clavier_souris] ; noms : un caractère tel qu'écrit sur le
// clavier, ou Espace, Entrée, Échap, Tab, Maj, Ctrl, Alt, Retour arrière, Flèche haut/bas/gauche/droite, F1 à F12,
// Clic gauche/droit/milieu, Souris 4, Souris 5. Le clavier n'est lu que fenêtre au premier plan, panneau F1 fermé ;
// la souris y est alors captive et invisible.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace game {

bool OptionsOpen();  // options.cpp

bool g_clavier_souris = true;
uint32_t g_souris_sensibilite = 100;  // %
bool g_souris_inverser_y = false;

namespace {

// une commande de la manette et les touches qui la donnent
struct Commande {
    const char* cle;               // nom dans settings.toml
    std::vector<uint16_t> defaut;  // codes de balayage (0xE000 | étendu), ou 0xF000 | bouton virtuel de la souris
    std::vector<int> vk;           // touches virtuelles retenues
    std::vector<std::string> noms;
};

constexpr uint16_t Souris(int vk) { return uint16_t(0xF000 | vk); }

// ordre fixe : les indices servent plus bas
enum { kHaut, kBas, kGauche, kDroite, kMarcher, kA, kB, kX, kY, kNoir, kBlanc, kGachetteG, kGachetteD, kStart, kBack,
       kCroixHaut, kCroixBas, kCroixGauche, kCroixDroite, kStickG, kStickD, kNombre };

std::vector<Commande>& Commandes() {
    static std::vector<Commande> c = {
        {"haut", {0x11}},
        {"bas", {0x1F}},
        {"gauche", {0x1E}},
        {"droite", {0x20}},
        {"marcher", {0x2A}},                     // Maj gauche : le stick à moitié
        {"a", {0x39, 0x1C}},                     // Espace, Entrée : sauter, valider
        {"b", {Souris(VK_LBUTTON), 0x12}},       // E : action, attaque, retour dans les menus
        {"x", {Souris(VK_RBUTTON), 0x13}},       // R
        {"y", {0x21}},                           // F
        {"noir", {0x03}},                        // 2
        {"blanc", {0x02}},                       // 1
        {"gachette_gauche", {0x1D, 0x2E}},       // Ctrl gauche, C : s'accroupir, passer les cinématiques
        {"gachette_droite", {Souris(VK_MBUTTON), 0x10}},  // A
        {"start", {0x01}},                       // Échap
        {"back", {0x0E}},                        // Retour arrière
        {"croix_haut", {0xE048}},
        {"croix_bas", {0xE050}},
        {"croix_gauche", {0xE04B}},
        {"croix_droite", {0xE04D}},
        {"clic_stick_gauche", {0x2F}},           // V
        {"clic_stick_droit", {Souris(VK_XBUTTON1)}},
    };
    return c;
}

struct Nom {
    const char* nom;
    int vk;
};
constexpr Nom kNoms[] = {
    {"Espace", VK_SPACE}, {"Entrée", VK_RETURN}, {"Échap", VK_ESCAPE}, {"Tab", VK_TAB}, {"Maj", VK_LSHIFT},
    {"Maj droite", VK_RSHIFT}, {"Ctrl", VK_LCONTROL}, {"Ctrl droit", VK_RCONTROL}, {"Alt", VK_LMENU},
    {"Retour arrière", VK_BACK}, {"Flèche haut", VK_UP}, {"Flèche bas", VK_DOWN}, {"Flèche gauche", VK_LEFT},
    {"Flèche droite", VK_RIGHT}, {"Inser", VK_INSERT}, {"Suppr", VK_DELETE}, {"Clic gauche", VK_LBUTTON},
    {"Clic droit", VK_RBUTTON}, {"Clic milieu", VK_MBUTTON}, {"Souris 4", VK_XBUTTON1}, {"Souris 5", VK_XBUTTON2},
    {"F2", VK_F2}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
};

std::wstring Large(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size())));
    return w;
}
std::string Etroit(const std::wstring& w) {
    std::string s(w.size() * 4, '\0');
    s.resize(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr));
    return s;
}

// le nom d'une touche par défaut sur le clavier de l'utilisateur
std::string NomDefaut(uint16_t code) {
    int vk = (code & 0xF000) == 0xF000 ? code & 0xFF : int(MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX));
    for (const Nom& n : kNoms)
        if (n.vk == vk) return n.nom;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));  // pas « & » pour 1
    wchar_t c = wchar_t(MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF);
    return c ? Etroit(std::wstring(1, wchar_t(std::towupper(c)))) : std::string();
}

// la touche virtuelle d'un nom, 0 inconnu
int Touche(const std::string& nom) {
    for (const Nom& n : kNoms)
        if (nom == n.nom) return n.vk;
    std::wstring w = Large(nom);
    if (w.size() != 1) return 0;
    SHORT r = VkKeyScanW(wchar_t(std::towlower(w[0])));
    return r == -1 ? 0 : r & 0xFF;
}

void Retenir(Commande& c, std::vector<std::string> noms) {
    c.noms.clear();
    c.vk.clear();
    for (auto& n : noms) {
        int vk = Touche(n);
        if (!vk) {
            LOGW("clavier : touche « %s » inconnue pour %s", n.c_str(), c.cle);
            continue;
        }
        c.noms.push_back(n);
        c.vk.push_back(vk);
    }
}

void Defauts() {
    for (Commande& c : Commandes()) {
        if (!c.vk.empty() || !c.noms.empty()) continue;
        std::vector<std::string> noms;
        for (uint16_t code : c.defaut)
            if (std::string n = NomDefaut(code); !n.empty()) noms.push_back(n);
        Retenir(c, noms);
    }
}

// la souris : déplacements bruts reçus par la fenêtre, convertis en vitesse
std::atomic<int64_t> g_dx{0}, g_dy{0};
HWND g_hwnd = nullptr;
bool g_capture = false;

bool Active() { return g_clavier_souris && g_hwnd && GetForegroundWindow() == g_hwnd && !OptionsOpen(); }

bool Appuye(const Commande& c) {
    for (int vk : c.vk)
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

void Capturer(bool oui) {
    if (oui) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);  // à chaque fois : la fenêtre a pu bouger ou changer de taille
    } else if (g_capture) {
        ClipCursor(nullptr);
    }
    g_capture = oui;
}

int16_t Axe(float v) { return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f); }

}  // namespace

void ClavierCharger(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    if (const toml::table* cs = t["clavier_souris"].as_table()) {
        g_clavier_souris = (*cs)["actif"].value_or(g_clavier_souris);
        g_souris_sensibilite = std::clamp<uint32_t>((*cs)["sensibilite"].value_or(g_souris_sensibilite), 10, 400);
        g_souris_inverser_y = (*cs)["inverser_y"].value_or(g_souris_inverser_y);
        for (Commande& c : Commandes())
            if (const toml::array* a = (*cs)[c.cle].as_array()) {
                std::vector<std::string> noms;
                for (auto& e : *a)
                    if (auto s = e.value<std::string>()) noms.push_back(*s);
                Retenir(c, noms);
                if (c.vk.empty()) c.noms = {""};  // vide voulu : pas de touche, pas de défaut
            }
    }
    Defauts();
}

void ClavierEnregistrer(void* table) {
    Defauts();
    toml::table cs{{"actif", g_clavier_souris},
                   {"sensibilite", int64_t(g_souris_sensibilite)},
                   {"inverser_y", g_souris_inverser_y}};
    for (Commande& c : Commandes()) {
        toml::array a;
        for (auto& n : c.noms)
            if (!n.empty()) a.push_back(n);
        cs.insert(c.cle, a);
    }
    static_cast<toml::table*>(table)->insert_or_assign("clavier_souris", cs);
}

// la fenêtre est créée : la souris y envoie ses déplacements bruts
void ClavierFenetre(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    RAWINPUTDEVICE d{0x01, 0x02, 0, g_hwnd};  // pointeur générique : la souris
    if (!RegisterRawInputDevices(&d, 1, sizeof(d))) LOGW("clavier : souris brute indisponible (%lu)", GetLastError());
    Defauts();
}

// fil de la fenêtre : déplacements de la souris, curseur caché quand elle sert au jeu
bool ClavierMessage(uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (msg == WM_INPUT) {
        RAWINPUT ri;
        UINT n = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &n, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && Active()) {
            g_dx += ri.data.mouse.lLastX;
            g_dy += ri.data.mouse.lLastY;
        }
        return false;  // DefWindowProc libère l'entrée
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && g_capture) {
        SetCursor(nullptr);
        *result = TRUE;
        return true;
    }
    return false;
}

// manette 1 : les touches et la souris s'ajoutent à ce que donne la manette
void ClavierSouris(int port, input::Pad& pad) {
    static const bool essai = std::getenv("XBE_AUTOPAD") != nullptr;  // essais automatiques : souris laissée libre
    if (port != 0 || !g_clavier_souris || essai) return;
    pad.connected = true;  // une manette 1 toujours là, même sans manette branchée
    bool active = Active();
    {
        // la zone du curseur suit la fenêtre, revue deux fois par seconde
        static std::mutex verrou;
        static ULONGLONG revue = 0;
        std::lock_guard lock(verrou);
        ULONGLONG now = GetTickCount64();
        if (active != g_capture || (active && now >= revue)) {
            Capturer(active);
            revue = now + 500;
        }
    }
    if (!active) {
        g_dx = 0;
        g_dy = 0;
        return;
    }
    auto& c = Commandes();

    // déplacement : la direction des touches, réduite en marchant, prise si plus forte que le stick
    float mx = float(Appuye(c[kDroite])) - float(Appuye(c[kGauche]));
    float my = float(Appuye(c[kHaut])) - float(Appuye(c[kBas]));
    if (mx || my) {
        float k = (Appuye(c[kMarcher]) ? 0.45f : 1.0f) / std::sqrt(mx * mx + my * my);
        pad.lx = Axe(mx * k);
        pad.ly = Axe(my * k);
    }

    // caméra : la vitesse de la souris, lissée ; plusieurs lectures par image se partagent les mêmes déplacements
    {
        using clock = std::chrono::steady_clock;
        static std::mutex verrou;
        static clock::time_point avant = clock::now();
        static float vx = 0, vy = 0;
        std::lock_guard lock(verrou);
        auto now = clock::now();
        float dt = std::chrono::duration<float>(now - avant).count();
        if (dt >= 0.008f) {
            avant = now;
            dt = std::min(dt, 0.1f);
            // pleine déviation à 1500 points par seconde à 100 %
            float k = g_souris_sensibilite / 100.0f / 1500.0f / dt;
            float x = float(g_dx.exchange(0)) * k, y = float(g_dy.exchange(0)) * k;
            float a = std::min(dt / 0.035f, 1.0f);  // lissage sur 35 ms
            vx += (x - vx) * a;
            vy += (y - vy) * a;
        }
        // au-delà de la zone morte du jeu dès le moindre mouvement
        auto vers_stick = [](float v) {
            constexpr float kMort = 0.24f;
            if (std::fabs(v) < 0.01f) return 0.0f;
            return std::copysign(kMort + (1 - kMort) * std::min(std::fabs(v), 1.0f), v);
        };
        float sx = vers_stick(vx), sy = vers_stick(g_souris_inverser_y ? vy : -vy);
        if (std::fabs(sx) > std::fabs(pad.rx / 32767.0f)) pad.rx = Axe(sx);
        if (std::fabs(sy) > std::fabs(pad.ry / 32767.0f)) pad.ry = Axe(sy);
    }

    // boutons
    for (int i = 0; i < 8; i++)
        if (Appuye(c[kA + i])) pad.analog[i] = 0xFF;  // A B X Y noir blanc gâchettes, dans l'ordre de la manette
    constexpr int kNumerique[] = {kCroixHaut, kCroixBas, kCroixGauche, kCroixDroite, kStart, kBack, kStickG, kStickD};
    for (int i = 0; i < 8; i++)
        if (Appuye(c[kNumerique[i]])) pad.buttons |= uint16_t(1u << i);
}

}  // namespace game
