// Diagnostic de blocage : XBE_HANG=<secondes> journalise, au bout de ce délai, les registres et la pile invitée
// de chaque thread du jeu (ceux qui attendent dans le noyau ont un contexte stable).
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"

namespace game {

void StartHangWatch() {
    // XBE_CAPTURE_AT=<secondes> (avec XBE_CAPTURE_DIR) : capture d'une image complète, comme F3
    // XBE_CAPTURE_AT=<s>[,<s>...] : plusieurs captures dans la même exécution
    // <s>x<n> : n images consécutives à partir de s (ce qui clignote)
    if (const char* c = std::getenv("XBE_CAPTURE_AT")) {
        std::vector<std::pair<int, int>> at;
        for (const char* p = c; *p;) {
            int s = std::atoi(p), n = 1;
            while (*p && *p != ',' && *p != 'x') p++;
            if (*p == 'x') n = std::atoi(++p);
            at.push_back({s, n});
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
        std::thread([at] {
            auto start = std::chrono::steady_clock::now();
            for (auto [t, n] : at) {
                std::this_thread::sleep_until(start + std::chrono::seconds(t));
                for (int i = 0; i < n; i++) {
                    gpu::g_capture = true;
                    while (gpu::g_capture) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }).detach();
    }
    // XBE_RECORD_AT=<s>x<n> : les n images présentées à partir de s, dans <XBE_CAPTURE_DIR>\suite
    if (const char* r = std::getenv("XBE_RECORD_AT")) {
        int s = std::atoi(r), n = 60;
        if (const char* x = std::strchr(r, 'x')) n = std::atoi(x + 1);
        std::thread([s, n] {
            std::this_thread::sleep_for(std::chrono::seconds(s));
            gpu::g_record = n;
        }).detach();
    }
    const char* v = std::getenv("XBE_HANG");
    if (!v) return;
    int seconds = std::atoi(v);
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        for (const kernel::ThreadRef& t : kernel::Threads()) {
            if (!t->ctx) continue;
            const X86Context& c = *t->ctx;
            LOGE("hang: thread %X eax %08X ecx %08X edx %08X ebx %08X esp %08X ebp %08X esi %08X edi %08X", t->id, c.eax,
                 c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi);
            kernel::LogGuestStack(*t->ctx, 96);
        }
        // XBE_HANG_DUMP=adresse[,adresse...] : 16 mots de mémoire invitée à chaque adresse
        if (const char* d = std::getenv("XBE_HANG_DUMP")) {
            for (char* p = const_cast<char*>(d); *p;) {
                uint32_t a = uint32_t(std::strtoul(p, &p, 16));
                for (int i = 0; i < 16; i += 4)
                    LOGE("dump %08X: %08X %08X %08X %08X", a + i * 4, kernel::R32(a + i * 4), kernel::R32(a + i * 4 + 4),
                         kernel::R32(a + i * 4 + 8), kernel::R32(a + i * 4 + 12));
                if (*p == ',') p++;
            }
        }
        logx::Flush();
    }).detach();
}

}  // namespace game
