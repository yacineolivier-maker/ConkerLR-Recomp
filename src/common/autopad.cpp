// Manette automatique pour les essais sans joueur : XBE_AUTOPAD="secondes:bouton[:durée];..." appuie sur un
// bouton de la manette 1 à l'instant donné (durée 0,2 s par défaut). Boutons : a b x y black white lt rt start
// back up down left right, ou lx/ly/rx/ry=valeur (-32768..32767). Préfixe p2. à p4. : une autre manette, branchée dès
// le début (ex. 30:p2.start). Sans la variable, rien ne change.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/input.h"

namespace game {

namespace {

struct Press {
    double at, until;
    std::string what;
    int port;
};

const std::vector<Press>& Script() {
    static const std::vector<Press> presses = [] {
        std::vector<Press> out;
        const char* v = std::getenv("XBE_AUTOPAD");
        if (!v) return out;
        std::string s(v);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t end = s.find(';', pos);
            std::string item = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? s.size() : end + 1;
            size_t c1 = item.find(':');
            if (c1 == std::string::npos) continue;
            size_t c2 = item.find(':', c1 + 1);
            double at = std::atof(item.c_str());
            double len = c2 == std::string::npos ? 0.2 : std::atof(item.c_str() + c2 + 1);
            std::string what = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            int port = 0;
            if (what.size() > 3 && what[0] == 'p' && what[1] >= '1' && what[1] <= '4' && what[2] == '.') {
                port = what[1] - '1';
                what = what.substr(3);
            }
            out.push_back({at, at + len, what, port});
        }
        return out;
    }();
    return presses;
}

}  // namespace

void AutoPad(int port, input::Pad& pad) {
    const auto& script = Script();
    if (script.empty()) return;
    bool used = port == 0;
    for (const Press& p : script) used |= p.port == port;
    if (!used) return;
    static const auto start = std::chrono::steady_clock::now();
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    pad.connected = true;
    static const char* analog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    static const char* digital[] = {"up", "down", "left", "right", "start", "back", "ls", "rs"};
    for (const Press& p : script) {
        if (p.port != port || t < p.at || t >= p.until) continue;
        for (int i = 0; i < 8; i++)
            if (p.what == analog[i]) pad.analog[i] = 255;
        for (int i = 0; i < 8; i++)
            if (p.what == digital[i]) pad.buttons |= uint16_t(1u << i);
        if (size_t eq = p.what.find('='); eq != std::string::npos) {
            int v = std::atoi(p.what.c_str() + eq + 1);
            std::string axis = p.what.substr(0, eq);
            if (axis == "lx") pad.lx = int16_t(v);
            if (axis == "ly") pad.ly = int16_t(v);
            if (axis == "rx") pad.rx = int16_t(v);
            if (axis == "ry") pad.ry = int16_t(v);
        }
    }
}

}  // namespace game
